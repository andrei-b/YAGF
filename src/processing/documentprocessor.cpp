#include "blockfont.h"
#include "documentprocessor.h"
#include "overlapreading.h"
#include "pagegeometry.h"
#include "fontestimator.h"

#include "imageprocessor.h"
#include "textguidedanalysis.h"
#include "tesseractengine.h"
#include "mathocrengine.h"
#include "spellingformulas.h"
#include "smartblocks.h"
#include "symbolshape.h"
#include "specialistocr.h"
#include "pagesource.h"
#include "remoteocrengine.h"
#include "ocrreadings.h"
#include "recognitionrecorder.h"

#include <QFile>
#include <QJsonDocument>
#include <QDateTime>
#include <QFileInfo>
#include <QImageReader>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QTimer>
#include <QPainter>
#include <QUuid>
#include <QtConcurrentRun>
#include "symbolaugmentation.h"
#include "formulatext.h"

#include <algorithm>
#include <cmath>

namespace yagf::processing {

DocumentProcessor::DocumentProcessor(QObject *parent, const core::OcrDocument *sourceBook)
    : QObject(parent), book(sourceBook), engine(std::make_unique<TesseractEngine>())
{
    connect(&process, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus status) {
        const Job finishedJob = job;
        if (finishedJob == Job::None) return;
        if (cancellationRequested) {
            discardRenderedPage(finishedJob);
            cancellationRequested = false;
            job = Job::None;
            batchPages.clear();
            emit busyChanged(false);
            emit cancelled();
            return;
        }
        if (status != QProcess::NormalExit || exitCode != 0) {
            discardRenderedPage(finishedJob);
            const QString message = tr("Processing failed: %1")
                .arg(QString::fromUtf8(process.readAllStandardError()).trimmed());
            if (finishedJob == Job::BatchRender) finishBatchError(message);
            else { job = Job::None; emit busyChanged(false); emit failed(message); }
            return;
        }
        if (finishedJob == Job::DocumentRender || finishedJob == Job::BatchRender) {
            const int number = finishedJob == Job::BatchRender ? batchPages.at(batchIndex).number : requestedPage;
            if (adaptRenderedPage(number)) {
                const auto command = pageRenderCommand(sourcePath(number), sourcePage(number), pagePrefix(number), false, selectedRenderDpi(number));
                process.start(command.program, command.arguments);
                return;
            }
        }
        if (finishedJob == Job::BatchRender) {
            prepareBatchImage(pageFile(batchPages.at(batchIndex).number));
            return;
        }
        job = Job::None;
        emit busyChanged(false);
        if (finishedJob == Job::DocumentInfo) {
            const QString info = QString::fromUtf8(process.readAllStandardOutput());
            const QRegularExpression pagesPattern(QStringLiteral("(?:^|\\n)Pages:\\s*(\\d+)"));
            const auto match = pagesPattern.match(info);
            const int count = pageSourceKind(documentSource) == PageSourceKind::DjVu
                ? info.trimmed().toInt() : match.hasMatch() ? match.captured(1).toInt() : 0;
            if (count < 1 || count > 100000) emit failed(tr("Could not read a valid document page count."));
            else emit documentOpened(count);
        } else if (finishedJob == Job::DocumentRender) {
            readyDpis[requestedPage] = selectedRenderDpi(requestedPage);
            emit pageRendered(requestedPage, pageFile(requestedPage));
        }
    });
    connect(&process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            batchPages.clear();
            job = Job::None;
            emit busyChanged(false);
            emit failed(tr("Could not start %1.").arg(process.program()));
        }
    });
    connect(&ocrWatcher, &QFutureWatcher<OcrJobResult>::finished, this, [this] {
        if (job != Job::Ocr && job != Job::BatchOcr && job != Job::SplitBlocks) return;
        OcrJobResult result = ocrWatcher.result();
        const bool batch = job == Job::BatchOcr;
        const bool split = job == Job::SplitBlocks;
        if((!result.outcome || (cancellationRequested && !batch)) && !result.historyPage.recognitionHistory.runs.isEmpty())
            emit recognitionHistoryReady(result.historyPage);
        if (cancellationRequested || (!result.outcome && result.outcome.error().cancelled)) {
            // A batch page that completed while the stop request was in flight is
            // finished work: deliver it before reporting the interruption.
            const bool keepPage = batch && result.outcome.has_value();
            cancellationRequested = false;
            batchPages.clear();
            job = Job::None;
            emit busyChanged(false);
            if (keepPage) emit batchPageRecognized(std::move(*result.outcome), batchSourceSize);
            emit cancelled();
            return;
        }
        if (!result.outcome) {
            if (batch) finishBatchError(result.outcome.error().message);
            else {
                job = Job::None;
                emit busyChanged(false);
                emit failed(result.outcome.error().message);
            }
            return;
        }
        if (!result.warnings.isEmpty())
            emit specialistWarning(tr("Page %1, specialist OCR: %2").arg(result.outcome->number)
                                   .arg(result.warnings.join(QStringLiteral(" / "))));
        if (batch) {
            emit batchPageRecognized(std::move(*result.outcome), batchSourceSize);
            if (batchPages.isEmpty() || job != Job::BatchOcr) return;
            ++batchIndex;
            emit batchProgress(batchIndex, batchPages.size());
            QTimer::singleShot(0, this, &DocumentProcessor::startNextBatchPage);
        } else {
            job = Job::None;
            emit busyChanged(false);
            if (split) emit blocksSplit(std::move(*result.outcome));
            else if (!result.selected.isEmpty()) {
                // Publish only after every selected block succeeded. The UI merges each
                // crop in place, preserving intervening unselected words and corrections.
                const auto &page = *result.outcome;
                for (const auto &block : result.selected) {
                    core::OcrPage crop;
                    crop.number = page.number;
                    crop.id = page.id;
                    crop.engineId = page.engineId;
                    crop.language = page.language;
                    crop.renderDpi = page.renderDpi;
                    crop.ocrReadings = core::cropOcrReadings(page.ocrReadings,block.bounds,block.shape());
                    crop.recognitionHistory=page.recognitionHistory;
                    crop.recognitionEvidence=page.recognitionEvidence;
                    for(auto &reading:crop.recognitionEvidence) {
                        auto &area=reading.provenance.inputArea;
                        if(area.isEmpty())area=QRect(QPoint(),reading.provenance.imageSize);
                        area.translate(-block.bounds.topLeft());
                    }
                    crop.coordinates=page.coordinates;
                    if(crop.coordinates.isDefined())crop.coordinates.appendProjective("selected-block",QTransform::fromTranslate(-block.bounds.x(),-block.bounds.y()),block.bounds.size());
                    crop.width = crop.sourceWidth = block.bounds.width();
                    crop.height = crop.sourceHeight = block.bounds.height();
                    for (auto word : page.words) if (block.shape().contains(word.bounds.center())) {
                        word.bounds.translate(-block.bounds.topLeft());
                        for (auto &fragment : word.sourceBoxes) fragment.translate(-block.bounds.topLeft());
                        crop.words.append(std::move(word));
                    }
                    for (auto region : page.regions) if (block.shape().contains(region.bounds.center())) {
                        region.bounds.translate(-block.bounds.topLeft());
                        crop.regions.append(std::move(region));
                    }
                    crop.rebuildHierarchy();
                    emit recognized(std::move(crop), block.bounds.topLeft(), block.bounds, block.id);
                }
            } else emit recognized(std::move(*result.outcome), result.offset, result.region, result.blockId);
        }
    });
    connect(&thumbnailProcess, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus status) {
        if (nextThumbnail < 1 || nextThumbnail > thumbnailCount) return;
        const int completed = nextThumbnail++;
        const QString path = pageRenderCommand(sourcePath(completed), sourcePage(completed),
            temporaryDir.filePath(renderPrefix + QStringLiteral("thumb-") + QString::number(completed)), true).outputPath;
        if (status == QProcess::NormalExit && exitCode == 0 && QFileInfo::exists(path))
            emit thumbnailReady(completed, path);
        startNextThumbnail();
    });
}

DocumentProcessor::~DocumentProcessor()
{
    ocrStop.request_stop();
    if (ocrWatcher.isRunning()) ocrWatcher.waitForFinished();
    thumbnailCount = 0;
    for (QProcess *running : {&thumbnailProcess, &process}) {
        if (running->state() != QProcess::NotRunning) {
            running->kill();
            running->waitForFinished(2000);
        }
    }
}

bool DocumentProcessor::isBusy() const { return job != Job::None; }
bool DocumentProcessor::isRecognizing() const
{
    return job == Job::Ocr || job == Job::BatchOcr || !batchPages.isEmpty();
}
QString DocumentProcessor::engineId() const { return engine->id(); }
EngineCapabilities DocumentProcessor::engineCapabilities() const { return engine->capabilities(); }
OcrService DocumentProcessor::primaryOcrService() const
{
    OcrService service; service.provider=engine->id();
    if (service.provider.endsWith("-remote")) service.provider.chop(7);
    service.label=ocrProviderLabel(service.provider);
    if (const auto *remote=dynamic_cast<const RemoteOcrEngine *>(engine.get())) {service.url=remote->serviceUrl(); service.token=remote->sessionToken(); service.model=remote->modelName();service.profile=remote->modelProfile();service.protocol=remote->serviceProtocol();}
    if(service.profile)service.tasks=service.profile->tasks;
    else if (engine->capabilities().supportsFormulas) service.tasks={"ocr","formula"};
    return service;
}
OcrService DocumentProcessor::formulaOcrService() const
{
    return selectedFormulaService.provider.isEmpty() ? localFormulaService() : selectedFormulaService;
}
std::expected<void, QString> DocumentProcessor::setPageLayoutService(PageLayoutOptions options,bool enabled)
{
    if (isBusy()) return std::unexpected(QStringLiteral("Wait for recognition to finish before changing page layout settings."));
    if (!enabled) {pageLayoutService.reset();return {};}
    if (!validOcrServiceUrl(options.url) || !std::isfinite(options.threshold) || options.threshold<.1 || options.threshold>.95)
        return std::unexpected(QStringLiteral("Enter a valid page layout service URL and minimum score."));
    pageLayoutService=std::move(options);
    return {};
}
std::expected<void, QString> DocumentProcessor::setFormulaOcrService(OcrService service)
{
    if (isBusy()) return std::unexpected(QStringLiteral("Wait for recognition to finish before changing the formula engine."));
    if (!service.supportsFormulas()) return std::unexpected(QStringLiteral("This OCR service does not advertise formula recognition."));
    if (service.provider=="rapid-latex-ocr") formulaRecognizer.reset();
    else {
        auto recognizer=makeOcrServiceEngine(service);
        if (!recognizer) return std::unexpected(recognizer.error());
        formulaRecognizer=std::move(*recognizer);
    }
    selectedFormulaService=std::move(service);
    return {};
}
void DocumentProcessor::setComparisonEngines(std::vector<ComparisonEngine> engines)
{
    if (isBusy()) return;
    if (engines.size()>3) engines.resize(3);
    comparisonEngines=std::move(engines);
}

void DocumentProcessor::setSpecialists(std::unique_ptr<IOcrEngine> formulas, std::unique_ptr<IOcrEngine> prose)
{
    if (isBusy()) return;
    formulaEngine = std::move(formulas);
    proseEngine = std::move(prose);
}

void DocumentProcessor::setEngine(std::unique_ptr<IOcrEngine> replacement)
{
    if (isBusy() || !replacement) return;
    textEngine.reset();
    nativePreviousEngine.reset();
    engine = std::move(replacement);
}

void DocumentProcessor::setNativeEngine(std::unique_ptr<IOcrEngine> replacement)
{
    if (isBusy()) return;
    auto &target = textEngine ? textEngine : engine;
    if (replacement) {
        if (!nativePreviousEngine) nativePreviousEngine = std::move(target);
        target = std::move(replacement);
    } else if (nativePreviousEngine) target = std::move(nativePreviousEngine);
}

void DocumentProcessor::setMathMode(bool enabled)
{
    if (isBusy() || enabled == bool(textEngine)) return;
    if (enabled) {
        textEngine = std::move(engine);
        engine = std::make_unique<MathOcrEngine>();
    } else {
        engine = std::move(textEngine);
    }
}

void DocumentProcessor::start(Job requestedJob, const QString &program, const QStringList &arguments)
{
    if (isBusy()) return;
    if (QStandardPaths::findExecutable(program).isEmpty()) {
        emit failed(tr("%1 is not installed or is not on PATH.").arg(program));
        return;
    }
    job = requestedJob;
    cancellationRequested = false;
    emit busyChanged(true);
    process.start(program, arguments);
}

void DocumentProcessor::openDocument(const QString &path)
{
    if (isBusy()) return;
    resetSource();
    if (pageSourceKind(path) == PageSourceKind::DjVu &&
        (QStandardPaths::findExecutable("djvused").isEmpty() || QStandardPaths::findExecutable("ddjvu").isEmpty())) {
        emit failed(tr("DjVu support requires the DjVuLibre tools djvused and ddjvu."));
        return;
    }
    documentSource = path;
    renderPrefix = QUuid::createUuid().toString(QUuid::WithoutBraces) + QLatin1Char('-');
    const auto command = pageCountCommand(path);
    start(Job::DocumentInfo, command.program, command.arguments);
}

void DocumentProcessor::resetSource()
{
    batchVocabulary = {};
    nextEvidencePage = 1;
    thumbnailCount = 0;
    nextThumbnail = 0;
    if (thumbnailProcess.state() != QProcess::NotRunning) {
        thumbnailProcess.kill();
        thumbnailProcess.waitForFinished(2000);
    }
    documentSource.clear();
    renderPrefix = QUuid::createUuid().toString(QUuid::WithoutBraces) + QLatin1Char('-');
    renderedDpis.clear();
    readyDpis.clear();
    adaptivePilot = false;
}

void DocumentProcessor::startThumbnails(int pageCount)
{
    thumbnailCount = pageCount;
    nextThumbnail = 1;
    startNextThumbnail();
}

void DocumentProcessor::startNextThumbnail()
{
    if (nextThumbnail < 1 || nextThumbnail > thumbnailCount ||
        thumbnailProcess.state() != QProcess::NotRunning) return;
    const QString output = temporaryDir.filePath(renderPrefix + QStringLiteral("thumb-") +
                                                 QString::number(nextThumbnail));
    if (!isPagedSource(sourcePath(nextThumbnail))) {
        const int completed = nextThumbnail++;
        emit thumbnailReady(completed, sourcePath(completed));
        QMetaObject::invokeMethod(this, &DocumentProcessor::startNextThumbnail, Qt::QueuedConnection);
        return;
    }
    const auto command = pageRenderCommand(sourcePath(nextThumbnail), sourcePage(nextThumbnail), output, true);
    thumbnailProcess.start(command.program, command.arguments);
}

int DocumentProcessor::selectedRenderDpi(int number) const { return renderedDpis.value(number, 150); }
int DocumentProcessor::renderedDpi(int number) const { return readyDpis.value(number, 150); }

QString DocumentProcessor::sourcePath(int number) const
{
    return book ? book->sourceForPage(number).path : documentSource;
}

int DocumentProcessor::sourcePage(int number) const
{
    return book ? book->sourceForPage(number).page : number;
}

QString DocumentProcessor::pagePrefix(int number) const
{
    return temporaryDir.filePath(renderPrefix + QStringLiteral("page-%1-%2").arg(number).arg(selectedRenderDpi(number)));
}

QString DocumentProcessor::pageFile(int number) const
{
    if (!isPagedSource(sourcePath(number))) return sourcePath(number);
    return pageRenderCommand(sourcePath(number), sourcePage(number), pagePrefix(number), false, selectedRenderDpi(number)).outputPath;
}

void DocumentProcessor::selectRenderDpi(int number, int overrideDpi)
{
    const auto *page = book ? book->page(number) : nullptr;
    if (overrideDpi == -1) renderedDpis[number] = 150;
    else if (core::validRenderDpi(overrideDpi)) renderedDpis[number] = overrideDpi;
    else if (page) renderedDpis[number] = page->renderDpi;
    else renderedDpis[number] = book && !book->rendering.adaptive ? book->rendering.dpi : 150;
    adaptivePilot = overrideDpi == -1 || (!overrideDpi && !page && book && book->rendering.adaptive && selectedRenderDpi(number) == 150);
}

bool DocumentProcessor::adaptRenderedPage(int number)
{
    if (!adaptivePilot || !isPagedSource(sourcePath(number))) return false;
    adaptivePilot = false;
    const QImage pilot(pageFile(number));
    int dpi = ImageProcessor::recommendedRenderDpi(pilot);
    while (dpi > 150 && double(pilot.width()) * pilot.height() * dpi * dpi / (150.0 * 150.0) > 80000000)
        dpi = dpi == 400 ? 300 : dpi == 300 ? 200 : 150;
    if (dpi == 150 || pilot.isNull()) return false;
    renderedDpis[number] = dpi;
    return true;
}

void DocumentProcessor::renderDocumentPage(int pageNumber, int dpiOverride)
{
    if (isBusy() || sourcePath(pageNumber).isEmpty() || pageNumber < 1) return;
    requestedPage = pageNumber;
    selectRenderDpi(pageNumber, dpiOverride);
    if (QFileInfo::exists(pageFile(pageNumber))) adaptRenderedPage(pageNumber);
    if (QFileInfo::exists(pageFile(pageNumber))) {
        readyDpis[pageNumber] = selectedRenderDpi(pageNumber);
        emit pageRendered(pageNumber, pageFile(pageNumber)); return;
    }
    const auto command = pageRenderCommand(sourcePath(pageNumber), sourcePage(pageNumber), pagePrefix(pageNumber), false, selectedRenderDpi(pageNumber));
    start(Job::DocumentRender, command.program, command.arguments);
}

void DocumentProcessor::discardRenderedPage(Job finishedJob)
{
    if (finishedJob != Job::DocumentRender && finishedJob != Job::BatchRender) return;
    const int number = finishedJob == Job::BatchRender ? batchPages.value(batchIndex).number : requestedPage;
    if (isPagedSource(sourcePath(number))) QFile::remove(pageFile(number));
}

void DocumentProcessor::startOcrImage(const QString &path, const QImage &image,
                                      OcrOptions options, const QRect &region, bool batch, bool splitOnly, QVector<core::ImageRegion> selected, std::optional<OcrService> tableService, const QString &blockId)
{
    if ((!batch && isBusy()) || (batch && batchPages.isEmpty())) return;
    options.preserveFormatting = book && book->preserveFormatting;
    const bool unicodeFormulas=book && book->unicodeFormulas;
    options.progress=[this,number=options.pageNumber](const QString &stage,int complete,int total) {
        emit recognitionStage(number,stage,complete,total);
    };
    QVector<core::SymbolRule> symbolRules = book ? book->symbolRules : QVector<core::SymbolRule>{};
    if (auto shared = core::loadSymbolLibrary(); shared) {
        for (const auto &rule : *shared)
            if (!std::ranges::any_of(symbolRules, [&](const auto &r) { return r.id == rule.id; })) symbolRules.append(rule);
    }
    const bool pagedSource = book && isPagedSource(book->sourceForPage(options.pageNumber).path);
    const auto *savedPage = book ? book->page(options.pageNumber) : nullptr;
    const int jobDpi = pagedSource ? (batch ? selectedRenderDpi(options.pageNumber) : renderedDpi(options.pageNumber)) : 150;
    const QSize savedImageSize = savedPage ? QSize(savedPage->width, savedPage->height) : QSize{};
    auto savedCoordinates=batch?batchCoordinates:(savedPage?savedPage->coordinates:core::CoordinatePipeline{});
    auto preprocessing=batch?batchPages.at(batchIndex).preprocessing:(savedPage?savedPage->preprocessing:core::PreprocessSettings{});
    const QString sourcePath=book?book->sourceForPage(options.pageNumber).path:path;
    const int savedDpi = savedPage ? savedPage->renderDpi : jobDpi;
    if (book && engine->id() == "tesseract") {
        options.language = TesseractEngine::modelLanguage(options.language);
        if (!batch || batchIndex == 0 || vocabularyLanguage != options.language ||
            nextEvidencePage > options.pageNumber) {
            batchVocabulary = {};
            vocabularyLanguage = options.language;
            nextEvidencePage = 1;
        }
        // In a sparse project a high page number can have very few recognized predecessors.
        for (const auto &page : book->pages) {
            if (page.number >= nextEvidencePage && page.number < options.pageNumber &&
                TesseractEngine::modelLanguage(page.language) == options.language) batchVocabulary.addPage(page);
        }
        nextEvidencePage = options.pageNumber;
        options.userWords = batchVocabulary.terms();
    }
    QVector<QRect> formulaRegions;
    if (book && formulaEngine && engine->id() == "tesseract") {
        if (const auto *previous = book->page(options.pageNumber)) {
            for (const auto &area : previous->regions)
                if (area.type == "equation" || area.type == "formula")
                    formulaRegions.append(area.bounds);
        }
    }
    const bool wasBusy = isBusy();
    const bool localText = engine->id() == "tesseract";
    const bool rawOutput = !tableService && localText && book && book->useRawOcrOutput;
    const auto layoutService=pageLayoutService;
    const bool automaticLayout=layoutService && localText && !rawOutput && !splitOnly && selected.isEmpty() && region.isEmpty();
    const QString deferredCompanion=formulaRecognizer ? selectedFormulaService.key() : QString();
    const auto textService = primaryOcrService();
    const auto formulaService = formulaOcrService();
    const auto endpoint = [](const OcrService &service) {
        auto url = service.url.adjusted(QUrl::NormalizePathSegments | QUrl::StripTrailingSlash);
        // Both base URL forms address the same OpenAI-compatible worker.
        if (service.provider == "paddleocr-vl" && url.path().endsWith("/v1"))
            url.setPath(url.path().chopped(3));
        return url;
    };
    const bool sharedEndpoint = !textService.url.isEmpty() &&
        endpoint(textService) == endpoint(formulaService);
    const bool parallelFormulas = textService.key() != formulaService.key() && !sharedEndpoint;
    QVector<core::ImageRegion> blocks = selected;
    if (blocks.isEmpty() && savedPage) {
        const bool newLanguage = localText && region.isEmpty() && !savedPage->language.isEmpty() &&
            TesseractEngine::modelLanguage(savedPage->language) != options.language;
        for (const auto &block : savedPage->imageRegions) {
            if (block.autoDetected && (splitOnly || (newLanguage && block.detectedLabel.isEmpty()))) continue;
            if (!region.isEmpty() && (block.bounds != region || (!blockId.isEmpty() && block.id != blockId))) continue;
            blocks.append(block);
        }
    }
    const bool manuallyTyped = std::ranges::any_of(blocks, [](const auto &block) {
        return !block.notText && !block.autoDetected && !block.content.isEmpty();
    });
    const bool automatic = localText && !splitOnly && selected.isEmpty() && !manuallyTyped && ((!formulaEngine && !proseEngine) || formulaRecognizer);
    const bool typedFormulas = formulaRecognizer && std::ranges::any_of(blocks, [](const auto &block) {
        return !block.notText && (block.content=="math" || block.content=="mixed");
    });
    const bool smart = splitOnly || (!tableService && !rawOutput && (automatic || automaticLayout ||
        (manuallyTyped && engine->id() != "rapid-latex-ocr") || (localText && !selected.isEmpty()) ||
        (engine->id()!="rapid-latex-ocr" && typedFormulas)));
    job = splitOnly ? Job::SplitBlocks : (batch ? Job::BatchOcr : Job::Ocr);
    cancellationRequested = false;
    ocrStop = std::stop_source{};
    const std::stop_token token = ocrStop.get_token();
    if (!wasBusy) emit busyChanged(true);
    const bool deskewLayout = splitOnly || automaticLayout || (smart && automatic && region.isEmpty() && selected.isEmpty());
    const auto layoutBase = deskewLayout && savedPage ? *savedPage : core::OcrPage{};
    const auto rawPath = pagedSource ? pageFile(options.pageNumber) : sourcePath;
    ocrWatcher.setFuture(QtConcurrent::run([this, path, image, options, region, token, formulaRegions, pagedSource, jobDpi, savedImageSize, savedDpi, batch, blocks, smart, automatic, localText, splitOnly, selected, rawOutput, symbolRules, unicodeFormulas, layoutService, automaticLayout, deferredCompanion, parallelFormulas, savedCoordinates, preprocessing, sourcePath, rawPath, layoutBase, deskewLayout, tableService]() mutable {
        const auto startedUtc=QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        QElapsedTimer elapsed;elapsed.start();QImage auditInput;core::OcrPage auditPage;
        auto perform=[&]() -> OcrJobResult {
        OcrJobResult result;
        result.selected = selected;
        QImage input = image;
        if (input.isNull()) {
            QImageReader reader(path);
            reader.setAutoTransform(true);
            input = reader.read();
            if (input.isNull()) {
                result.outcome = std::unexpected(OcrError{
                    QStringLiteral("Could not read OCR image: %1").arg(reader.errorString())});
                return result;
            }
        }
        const int dpi = pagedSource && !batch && savedImageSize == input.size() ? savedDpi : jobDpi;
        QSize sourceSize = input.size();
        const auto inputSize = input.size();
        const auto originalSettings = preprocessing;
        const auto originalCoordinates = savedCoordinates;
        if (deskewLayout) {
            QImage raw = input;
            if (!preprocessing.isIdentity()) {
                QImageReader reader(rawPath);
                reader.setAutoTransform(true);
                raw = reader.read();
            }
            auto base = layoutBase;
            base.number = options.pageNumber;
            base.imageRegions = blocks;
            if (!base.width) {
                base.width = input.width(); base.height = input.height();
                base.sourceWidth = raw.width(); base.sourceHeight = raw.height();
            }
            base.preprocessing = preprocessing;
            base.coordinates = savedCoordinates;
            if (options.progress) options.progress(QStringLiteral("Checking text-line skew"), 0, 0);
            auto layout = prepareLayoutPage(raw, input, std::move(base),
                pageSourceKind(sourcePath) == PageSourceKind::Pdf ? dpi : 0, options.language, token);
            if (!layout) {
                result.outcome = std::unexpected(OcrError{layout.error(), token.stop_requested()});
                return result;
            }
            input = std::move(layout->image);
            blocks = std::move(layout->page.imageRegions);
            savedCoordinates = layout->page.coordinates;
            preprocessing = layout->page.preprocessing;
            sourceSize = raw.size();
        }
        options.history=std::make_shared<RecognitionRecorder>(recognitionSourceHash(sourcePath,token),core::preprocessingEvidence(preprocessing,dpi));
        auditInput=input;
        auditPage.id=QStringLiteral("page-%1").arg(options.pageNumber);auditPage.number=options.pageNumber;
        auditPage.width=inputSize.width();auditPage.height=inputSize.height();
        auditPage.sourceWidth=sourceSize.width();auditPage.sourceHeight=sourceSize.height();
        auditPage.renderDpi=dpi;auditPage.preprocessing=originalSettings;auditPage.coordinates=originalCoordinates;
        options.historyCoordinates=savedCoordinates;
        if(!options.historyCoordinates.isDefined()) {
            options.historyCoordinates.sourceSize=input.size();options.historyCoordinates.sourceFrame="ocr-input";
        }
        if (!region.isEmpty()) {
            const QRect bounded = region.intersected(input.rect());
            if (bounded.isEmpty()) {
                result.outcome = std::unexpected(OcrError{
                    QStringLiteral("The selected region is outside the image.")});
                return result;
            }
            result.offset = bounded.topLeft();
            result.region = bounded;
            if (blocks.size() == 1) result.blockId = blocks.first().id;
            options=croppedOcrOptions(options,bounded);
            const auto owner = std::ranges::find(blocks,bounded,&core::ImageRegion::bounds);
            input = owner == blocks.end() ? input.copy(bounded) : owner->crop(input);
        }
        if (pagedSource && options.preserveFormatting) {
            input.setDotsPerMeterX(qRound(dpi / 0.0254));
            input.setDotsPerMeterY(qRound(dpi / 0.0254));
        }
        QVector<QRegion> comparisonAreas;
        QImage comparisonImage=input;
        if (selected.isEmpty()) {
            comparisonAreas.append(input.rect());
            for (const auto &block:blocks) if (block.notText) {
                if (comparisonImage.cacheKey()==input.cacheKey()) comparisonImage=input.copy();
                QPainter painter(&comparisonImage); for (QRect part : block.shape()) painter.fillRect(part.translated(-result.offset),Qt::white);
            }
        } else for (const auto &block:selected) if (!block.notText) {
            const auto area=block.shape().translated(-result.offset).intersected(input.rect());
            if (!area.isEmpty()) comparisonAreas.append(area);
        }
        std::vector<ComparisonEngine> none;
        // Formula requests take priority over a full-page comparison using the
        // same server; a single inference slot must not starve the primary OCR.
        ParallelOcrRun companions(splitOnly || tableService ? none : comparisonEngines,comparisonImage,options,comparisonAreas,token,deferredCompanion,false);
        if (tableService) {
            auto reader = makeOcrServiceEngine(*tableService);
            if (!reader) { result.outcome = std::unexpected(OcrError{reader.error()}); return result; }
            options.task = "table";
            options.language = tableService->language.isEmpty() ? QStringLiteral("auto") : tableService->language;
            result.outcome = (*reader)->recognize(input, options, token);
            if (result.outcome && !std::ranges::any_of(result.outcome->regions,
                    [](const auto &r) { return r.type == "table"; }))
                result.outcome = std::unexpected(OcrError{tr("The recognition service returned no table.")});
        } else if (smart) {
            if (options.progress) options.progress(QStringLiteral("Detecting text and formula areas"),0,0);
            auto local = blocks;
            for (auto &block : local) block.bounds.translate(-result.offset);
            if (automaticLayout) {
                if (options.progress) options.progress(QStringLiteral("Analyzing AI page layout"),0,0);
                auto layoutOptions=options;layoutOptions.historyRole="layout";
                auto run=options.history->begin("ai-page-layout","auto",input,layoutOptions);
                run.preprocessing["layout_threshold"]=layoutService->threshold;
                run.preprocessingHash=core::sha256(QJsonDocument(run.preprocessing).toJson(QJsonDocument::Compact));
                QElapsedTimer timer;timer.start();
                QJsonObject output;auto layout=detectPageAreas(input,*layoutService,token,&output);
                if(layout)run.model=layout->model;
                options.history->finish(std::move(run),timer.elapsed(),layout?"succeeded":layout.error().cancelled?"cancelled":"failed",
                    layout?QString():layout.error().message,output);
                if (!layout) {
                    if (layout.error().cancelled || token.stop_requested()) {result.outcome=std::unexpected(layout.error());return result;}
                    result.warnings.append(QStringLiteral("AI page layout failed: %1 Used the existing blocks and local detection.").arg(layout.error().message));
                } else local=mergePageAreas(local,layout->areas,true);
            }
            result.outcome = recognizeSmartBlocks(input, options, local, splitOnly, token,
                selected.isEmpty(), nullptr, &result.warnings,
                (automatic || !localText) && engine->id()!="rapid-latex-ocr" ? engine.get() : nullptr,
                formulaRecognizer.get(), automatic, parallelFormulas);
        } else if (rawOutput && !selected.isEmpty()) {
            core::OcrPage merged;
            merged.id = QStringLiteral("page-%1").arg(options.pageNumber);
            merged.number = options.pageNumber;
            merged.width = merged.sourceWidth = input.width();
            merged.height = merged.sourceHeight = input.height();
            merged.language = options.language;
            merged.imageRegions = selected;
            int lastBlock = 0;
            QVector<QVector<core::OcrWord>> cropWords;
            QVector<QRegion> cropShapes;
            for (const auto &block : selected) {
                if (block.notText) continue;
                const QRect crop = block.bounds.intersected(input.rect());
                if (crop.isEmpty()) {
                    result.outcome = std::unexpected(OcrError{QStringLiteral("The selected region is outside the image.")});
                    return result;
                }
                auto settings = croppedOcrOptions(options,crop);
                if (!block.language.isEmpty()) settings.language = TesseractEngine::modelLanguage(block.language);
                auto reading = engine->recognize(block.crop(input), settings, token);
                if (!reading) { result.outcome = std::unexpected(reading.error()); return result; }
                appendRecognitionEvidence(merged, *reading, crop.topLeft());
                const auto shape = block.shape();
                int maximumBlock = lastBlock;
                for (auto &word : reading->words) {
                    word.bounds.translate(crop.topLeft());
                    for (auto &box : word.sourceBoxes) box.translate(crop.topLeft());
                    word.atOcrCropEdge = core::touchesOcrCropEdge(word,shape);
                    word.block += lastBlock;
                    maximumBlock = std::max(maximumBlock, word.block);
                    word.id.clear(); word.sourceWordIds.clear();
                }
                for (int i=0; i<cropWords.size(); ++i)
                    core::reconcileOverlap(cropWords[i],reading->words,cropShapes[i] & shape,cropShapes[i],shape);
                cropShapes.append(shape);
                cropWords.append(std::move(reading->words));
                lastBlock = maximumBlock;
            }
            for (auto &words : cropWords) merged.words += std::move(words);
            result.outcome = std::move(merged);
        } else if (!rawOutput && engine->id()=="rapid-latex-ocr") {
            QVector<core::ImageRegion> scopes = selected;
            if (scopes.isEmpty()) scopes.append({"math-only",input.rect(),{},false,"math"});
            for (auto &scope : scopes) scope.content = "math";
            result.outcome = recognizeSmartBlocks(input, options, scopes, false, token, false,
                nullptr, &result.warnings, nullptr, formulaRecognizer.get());
            if (result.outcome) {
                result.outcome->imageRegions = blocks;
                for (auto &block : result.outcome->imageRegions) block.bounds.translate(-result.offset);
            }
        } else {
            result.outcome = engine->recognize(input, options, token);
        }
        if (result.outcome) {
            core::OcrPage &page = *result.outcome;
            if (rawOutput && selected.isEmpty()) page.words.removeIf([&](const auto &word) {
                return std::ranges::any_of(blocks, [&](const auto &block) {
                    return block.notText && block.shape().contains(word.bounds.center());
                });
            });
            if (!smart && !tableService) page.engineId = engine->id();
            page.renderDpi = dpi;
            if (deskewLayout) {
                page.sourceWidth = sourceSize.width();
                page.sourceHeight = sourceSize.height();
            }
            page.preprocessing=preprocessing;
            page.coordinates=savedCoordinates;
            if(!page.coordinates.isDefined())page.coordinates.sourceSize=input.size();
            else if(!region.isEmpty())page.coordinates.appendProjective("selected-region",QTransform::fromTranslate(-result.offset.x(),-result.offset.y()),input.size());
            if (page.number != options.pageNumber || page.width != input.width() ||
                page.height != input.height()) {
                result.outcome = std::unexpected(OcrError{
                    QStringLiteral("The OCR engine returned inconsistent page geometry.")});
                return result;
            }
            page.rebuildHierarchy();
            if (options.preserveFormatting) {
                if (options.progress) options.progress(QStringLiteral("Matching text fonts"),0,0);
                estimateTextFormatting(page, input, token);
            }
            const QRegion inputCrop = !region.isEmpty() && blocks.size()==1 ? blocks.first().shape().translated(-result.offset) : QRegion(input.rect());
            QSet<QString> ids;
            for (core::OcrWord &word : page.words) {
                if (!input.rect().contains(word.bounds) || word.bounds.isEmpty() ||
                    word.id.isEmpty() || ids.contains(word.id)) {
                    result.outcome = std::unexpected(OcrError{
                        QStringLiteral("The OCR engine returned invalid word geometry or IDs.")});
                    return result;
                }
                if (token.stop_requested()) {
                    result.outcome = std::unexpected(OcrError{{}, true});
                    return result;
                }
                if (engine->capabilities().supportsWordBoxes && !word.layoutBlock && !word.geometryInferred &&
                    core::isSingleSymbol(word.originalText))
                    word.symbolShape = symbolShape(input, word.bounds);
                if (!region.isEmpty()) word.atOcrCropEdge |= core::touchesOcrCropEdge(word,inputCrop);
                ids.insert(word.id);
            }
            if (!tableService && !rawOutput && !smart && engine->id() == "tesseract" && (formulaEngine || proseEngine)) {
                QVector<QRect> localAreas = formulaRegions;
                for (auto &area : localAreas) area.translate(-result.offset);
                result.warnings = collectSpecialistReadings(page, input, localAreas,
                    formulaEngine.get(), proseEngine.get(), token,options);
                if (token.stop_requested()) result.outcome = std::unexpected(OcrError{{}, true});
            }
            if (result.outcome && !tableService && !rawOutput && !splitOnly && engine->capabilities().supportsWordBoxes) {
                auto retries = collectSpellingFormulaReadings(page, input, token,options.progress,formulaRecognizer.get(),options);
                if (!retries) result.outcome = std::unexpected(retries.error());
                else result.warnings += *retries;
            }
            if (result.outcome && !tableService && !rawOutput && !splitOnly) {
                auto retries = collectFormulaTextReadings(page, input, options.language, token,options.progress,options);
                if (!retries) result.outcome = std::unexpected(retries.error());
                else result.warnings += *retries;
            }
            if (result.outcome && !tableService && !rawOutput && !splitOnly) {
                if (options.progress) options.progress(QStringLiteral("Applying learned symbol rules"),0,0);
                applySymbolRules(page, input, symbolRules, token);
            }
            if (result.outcome && !rawOutput && !splitOnly && unicodeFormulas)
                if (const int unsupported=core::setFormulaOutput(page,true);unsupported)
                    result.warnings.append(QStringLiteral("%1 formula(s) retained LaTeX because their Unicode conversion is unsupported.").arg(unsupported));
            if (result.outcome && !splitOnly) {
                auto fontBlocks = blocks;
                for (auto &block : fontBlocks) block.bounds.translate(-result.offset);
                core::normalizeBlockFontSizes(page, fontBlocks, !region.isEmpty(), token);
            }
            if (result.outcome && !tableService && !splitOnly && !comparisonEngines.empty()) {
                if (options.progress) options.progress(QStringLiteral("Waiting for companion OCR results"),0,comparisonEngines.size());
                page.ocrReadings=companions.finish();
                if (!deferredCompanion.isEmpty() && !token.stop_requested()) {
                    ParallelOcrRun deferred(comparisonEngines,comparisonImage,options,comparisonAreas,token,deferredCompanion);
                    page.ocrReadings+=deferred.finish();
                }
                for (const auto &reading:page.ocrReadings) if (!reading.error.isEmpty())
                    result.warnings.append(reading.label+": "+reading.error+QStringLiteral(" Kept the primary reading."));
            }
            if(result.outcome)core::attachRecognitionCoordinates(page);
            if (token.stop_requested()) result.outcome = std::unexpected(OcrError{{}, true});
        }
        return result;
        };
        auto result=perform();
        if(!auditInput.isNull()) {
            auto context=options;context.historyRole="pipeline";context.historyCoordinates=savedCoordinates;context.historyOffset={};
            auto run=options.history->begin("yagf-pipeline","YAGF",auditInput,context);run.startedUtc=startedUtc;
            options.history->finish(std::move(run),elapsed.elapsed(),result.outcome?"succeeded":result.outcome.error().cancelled?"cancelled":"failed",
                result.outcome?QString():result.outcome.error().message,{}, {},result.warnings);
        }
        if (options.history) auditPage.recognitionHistory=options.history->snapshot();
        result.historyPage=std::move(auditPage);
        if(result.outcome)result.outcome->recognitionHistory=result.historyPage.recognitionHistory;
        return result;
    }));
}

void DocumentProcessor::splitBlocks(const QString &imagePath, const QString &language, int pageNumber)
{
    startOcrImage(imagePath, {}, {.language = language, .pageNumber = pageNumber}, {}, false, true);
}

void DocumentProcessor::recognize(const QString &imagePath, const QString &language, int pageNumber)
{
    startOcrImage(imagePath, {}, {.language = language, .pageNumber = pageNumber}, {}, false);
}

void DocumentProcessor::recognizeTypedRegions(const QString &imagePath, const QString &language, int pageNumber,
                                              const QVector<core::ImageRegion> &selected)
{
    if (!selected.isEmpty())
        startOcrImage(imagePath, {}, {.language = language, .pageNumber = pageNumber}, {}, false, false, selected);
}

void DocumentProcessor::recognizeRegion(const QString &imagePath, const QString &language,
                                        int pageNumber, const QRect &region, std::optional<OcrService> tableService, const QString &blockId)
{
    if (region.isEmpty()) { emit failed(tr("Select an image region first.")); return; }
    if (!tableService && !engineCapabilities().supportsRegions) {
        emit failed(tr("The selected OCR engine does not support region recognition."));
        return;
    }
    startOcrImage(imagePath, {}, {.language = language, .pageNumber = pageNumber}, region, false, false, {}, std::move(tableService), blockId);
}

void DocumentProcessor::recognizePages(const QVector<BatchPageOptions> &pages)
{
    if (isBusy() || pages.isEmpty() || sourcePath(pages.first().number).isEmpty()) return;
    batchPages = pages;
    batchIndex = 0;
    job = Job::BatchRender;
    cancellationRequested = false;
    emit busyChanged(true);
    emit batchProgress(0, batchPages.size());
    startNextBatchPage();
}

void DocumentProcessor::startNextBatchPage()
{
    if (batchPages.isEmpty()) return;
    if (batchIndex >= batchPages.size()) {
        batchPages.clear();
        job = Job::None;
        emit busyChanged(false);
        emit batchFinished();
        return;
    }
    const int number = batchPages.at(batchIndex).number;
    selectRenderDpi(number);
    if (!isPagedSource(sourcePath(number))) {
        prepareBatchImage(sourcePath(number));
        return;
    }
    if (QFileInfo::exists(pageFile(number))) adaptRenderedPage(number);
    const auto command = pageRenderCommand(sourcePath(number), sourcePage(number), pagePrefix(number), false, selectedRenderDpi(number));
    const QString imagePath = command.outputPath;
    if (QFileInfo::exists(imagePath)) {
        prepareBatchImage(imagePath);
        return;
    }
    job = Job::BatchRender;
    process.start(command.program, command.arguments);
}

void DocumentProcessor::prepareBatchImage(const QString &path)
{
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QImage source = reader.read();
    if (source.isNull()) {
        finishBatchError(tr("Could not read rendered page %1.").arg(batchPages.at(batchIndex).number));
        return;
    }
    const auto processed = ImageProcessor::apply(source, batchPages.at(batchIndex).preprocessing);
    if (!processed) { finishBatchError(processed.error()); return; }
    readyDpis[batchPages.at(batchIndex).number] = selectedRenderDpi(batchPages.at(batchIndex).number);
    batchSourceSize = source.size();
    auto coordinates=ImageProcessor::coordinatePipeline(source.size(),batchPages.at(batchIndex).preprocessing,
        pageSourceKind(sourcePath(batchPages.at(batchIndex).number))==PageSourceKind::Pdf?selectedRenderDpi(batchPages.at(batchIndex).number):0);
    if(!coordinates){finishBatchError(coordinates.error());return;}batchCoordinates=*coordinates;
    startOcrImage({}, *processed,
        {.language = batchPages.at(batchIndex).language,
         .pageNumber = batchPages.at(batchIndex).number}, {}, true);
}

void DocumentProcessor::finishBatchError(const QString &message)
{
    batchPages.clear();
    job = Job::None;
    emit busyChanged(false);
    emit failed(message);
}

void DocumentProcessor::cancel()
{
    if (!isBusy()) return;
    cancellationRequested = true;
    if (job == Job::Ocr || job == Job::BatchOcr || job == Job::SplitBlocks) {
        ocrStop.request_stop();
        if (!ocrWatcher.isFinished()) return;
    } else if (process.state() != QProcess::NotRunning) {
        process.kill();
        return;
    }
    cancellationRequested = false;
    batchPages.clear();
    job = Job::None;
    emit busyChanged(false);
    emit cancelled();
}

} // namespace yagf::processing

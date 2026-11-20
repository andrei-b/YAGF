#include "documentprocessor.h"
#include "fontestimator.h"

#include "imageprocessor.h"
#include "tesseractengine.h"
#include "mathocrengine.h"
#include "spellingformulas.h"
#include "smartblocks.h"
#include "symbolshape.h"
#include "specialistocr.h"
#include "pagesource.h"

#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>
#include <QtConcurrentRun>
#include "symbolaugmentation.h"
#include "formulatext.h"

#include <algorithm>

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
                const auto command = pageRenderCommand(documentSource, number, pagePrefix(number), false, selectedRenderDpi(number));
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
        if (cancellationRequested || (!result.outcome && result.outcome.error().cancelled)) {
            cancellationRequested = false;
            batchPages.clear();
            job = Job::None;
            emit busyChanged(false);
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
                    crop.width = crop.sourceWidth = block.bounds.width();
                    crop.height = crop.sourceHeight = block.bounds.height();
                    for (auto word : page.words) if (block.bounds.contains(word.bounds.center())) {
                        word.bounds.translate(-block.bounds.topLeft());
                        for (auto &fragment : word.sourceBoxes) fragment.translate(-block.bounds.topLeft());
                        crop.words.append(std::move(word));
                    }
                    for (auto region : page.regions) if (block.bounds.contains(region.bounds.center())) {
                        region.bounds.translate(-block.bounds.topLeft());
                        crop.regions.append(std::move(region));
                    }
                    crop.rebuildHierarchy();
                    emit recognized(std::move(crop), block.bounds.topLeft(), block.bounds);
                }
            } else emit recognized(std::move(*result.outcome), result.offset, result.region);
        }
    });
    connect(&thumbnailProcess, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus status) {
        if (nextThumbnail < 1 || nextThumbnail > thumbnailCount) return;
        const int completed = nextThumbnail++;
        const QString path = pageRenderCommand(documentSource, completed,
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
    engine = std::move(replacement);
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
    renderPrefix.clear();
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
    const auto command = pageRenderCommand(documentSource, nextThumbnail, output, true);
    thumbnailProcess.start(command.program, command.arguments);
}

int DocumentProcessor::selectedRenderDpi(int number) const { return renderedDpis.value(number, 150); }
int DocumentProcessor::renderedDpi(int number) const { return readyDpis.value(number, 150); }

QString DocumentProcessor::pagePrefix(int number) const
{
    return temporaryDir.filePath(renderPrefix + QStringLiteral("page-%1-%2").arg(number).arg(selectedRenderDpi(number)));
}

QString DocumentProcessor::pageFile(int number) const
{
    return pageRenderCommand(documentSource, number, pagePrefix(number), false, selectedRenderDpi(number)).outputPath;
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
    if (!adaptivePilot) return false;
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
    if (isBusy() || documentSource.isEmpty() || pageNumber < 1) return;
    requestedPage = pageNumber;
    selectRenderDpi(pageNumber, dpiOverride);
    if (QFileInfo::exists(pageFile(pageNumber))) adaptRenderedPage(pageNumber);
    if (QFileInfo::exists(pageFile(pageNumber))) {
        readyDpis[pageNumber] = selectedRenderDpi(pageNumber);
        emit pageRendered(pageNumber, pageFile(pageNumber)); return;
    }
    const auto command = pageRenderCommand(documentSource, pageNumber, pagePrefix(pageNumber), false, selectedRenderDpi(pageNumber));
    start(Job::DocumentRender, command.program, command.arguments);
}

void DocumentProcessor::discardRenderedPage(Job finishedJob)
{
    if (finishedJob != Job::DocumentRender && finishedJob != Job::BatchRender) return;
    const int number = finishedJob == Job::BatchRender ? batchPages.value(batchIndex).number : requestedPage;
    QFile::remove(pageFile(number));
}

void DocumentProcessor::startOcrImage(const QString &path, const QImage &image,
                                      OcrOptions options, const QRect &region, bool batch, bool splitOnly, QVector<core::ImageRegion> selected)
{
    if ((!batch && isBusy()) || (batch && batchPages.isEmpty())) return;
    options.preserveFormatting = book && book->preserveFormatting;
    const bool unicodeFormulas=book && book->unicodeFormulas;
    QVector<core::SymbolRule> symbolRules = book ? book->symbolRules : QVector<core::SymbolRule>{};
    if (auto shared = core::loadSymbolLibrary(); shared) {
        for (const auto &rule : *shared)
            if (!std::ranges::any_of(symbolRules, [&](const auto &r) { return r.id == rule.id; })) symbolRules.append(rule);
    }
    const bool pagedSource = book && isPagedSource(book->sourcePath);
    const auto *savedPage = book ? book->page(options.pageNumber) : nullptr;
    const int jobDpi = pagedSource ? (batch ? selectedRenderDpi(options.pageNumber) : renderedDpi(options.pageNumber)) : 150;
    const QSize savedImageSize = savedPage ? QSize(savedPage->width, savedPage->height) : QSize{};
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
    const bool rawOutput = localText && book && book->useRawOcrOutput;
    QVector<core::ImageRegion> blocks = selected;
    if (blocks.isEmpty() && savedPage) {
        const bool newLanguage = localText && region.isEmpty() && !savedPage->language.isEmpty() &&
            TesseractEngine::modelLanguage(savedPage->language) != options.language;
        for (const auto &block : savedPage->imageRegions) {
            if ((splitOnly || newLanguage) && block.autoDetected) continue;
            if (!region.isEmpty() && block.bounds != region) continue;
            blocks.append(block);
        }
    }
    const bool manuallyTyped = std::ranges::any_of(blocks, [](const auto &block) {
        return !block.notText && !block.autoDetected && !block.content.isEmpty();
    });
    const bool automatic = localText && !splitOnly && selected.isEmpty() && !manuallyTyped && !formulaEngine && !proseEngine;
    const bool smart = splitOnly || (!rawOutput && (automatic ||
        (manuallyTyped && engine->id() != "rapid-latex-ocr") || (localText && !selected.isEmpty())));
    job = splitOnly ? Job::SplitBlocks : (batch ? Job::BatchOcr : Job::Ocr);
    cancellationRequested = false;
    ocrStop = std::stop_source{};
    const std::stop_token token = ocrStop.get_token();
    if (!wasBusy) emit busyChanged(true);
    ocrWatcher.setFuture(QtConcurrent::run([this, path, image, options, region, token, formulaRegions, pagedSource, jobDpi, savedImageSize, savedDpi, batch, blocks, smart, automatic, splitOnly, selected, rawOutput, symbolRules, unicodeFormulas] {
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
        if (!region.isEmpty()) {
            const QRect bounded = region.intersected(input.rect());
            if (bounded.isEmpty()) {
                result.outcome = std::unexpected(OcrError{
                    QStringLiteral("The selected region is outside the image.")});
                return result;
            }
            result.offset = bounded.topLeft();
            result.region = bounded;
            input = input.copy(bounded);
        }
        if (pagedSource && options.preserveFormatting) {
            input.setDotsPerMeterX(qRound(dpi / 0.0254));
            input.setDotsPerMeterY(qRound(dpi / 0.0254));
        }
        if (smart) {
            auto local = blocks;
            for (auto &block : local) block.bounds.translate(-result.offset);
            std::optional<core::OcrPage> baseline;
            if (automatic) {
                auto reading = engine->recognize(input, options, token);
                if (!reading) {
                    result.outcome = std::unexpected(reading.error());
                    return result;
                }
                baseline = std::move(*reading);
            }
            result.outcome = recognizeSmartBlocks(input, options, local, splitOnly, token,
                selected.isEmpty(), baseline ? &*baseline : nullptr, &result.warnings);
        } else if (rawOutput && !selected.isEmpty()) {
            core::OcrPage merged;
            merged.id = QStringLiteral("page-%1").arg(options.pageNumber);
            merged.number = options.pageNumber;
            merged.width = merged.sourceWidth = input.width();
            merged.height = merged.sourceHeight = input.height();
            merged.language = options.language;
            merged.imageRegions = selected;
            int lastBlock = 0;
            for (const auto &block : selected) {
                if (block.notText) continue;
                const QRect crop = block.bounds.intersected(input.rect());
                if (crop.isEmpty()) {
                    result.outcome = std::unexpected(OcrError{QStringLiteral("The selected region is outside the image.")});
                    return result;
                }
                auto settings = options;
                if (!block.language.isEmpty()) settings.language = TesseractEngine::modelLanguage(block.language);
                auto reading = engine->recognize(input.copy(crop), settings, token);
                if (!reading) { result.outcome = std::unexpected(reading.error()); return result; }
                int maximumBlock = lastBlock;
                for (auto word : reading->words) {
                    word.bounds.translate(crop.topLeft());
                    for (auto &box : word.sourceBoxes) box.translate(crop.topLeft());
                    word.block += lastBlock;
                    maximumBlock = std::max(maximumBlock, word.block);
                    word.id.clear(); word.sourceWordIds.clear();
                    merged.words.append(std::move(word));
                }
                lastBlock = maximumBlock;
            }
            result.outcome = std::move(merged);
        } else {
            result.outcome = engine->recognize(input, options, token);
        }
        if (result.outcome) {
            core::OcrPage &page = *result.outcome;
            if (rawOutput && selected.isEmpty()) page.words.removeIf([&](const auto &word) {
                return std::ranges::any_of(blocks, [&](const auto &block) {
                    return block.notText && block.bounds.contains(word.bounds.center());
                });
            });
            if (!smart) page.engineId = engine->id();
            page.renderDpi = dpi;
            if (page.number != options.pageNumber || page.width != input.width() ||
                page.height != input.height()) {
                result.outcome = std::unexpected(OcrError{
                    QStringLiteral("The OCR engine returned inconsistent page geometry.")});
                return result;
            }
            page.rebuildHierarchy();
            if (options.preserveFormatting) estimateTextFormatting(page, input, token);
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
                if (engine->capabilities().supportsWordBoxes && core::isSingleSymbol(word.originalText))
                    word.symbolShape = symbolShape(input, word.bounds);
                ids.insert(word.id);
            }
            if (!rawOutput && !smart && engine->id() == "tesseract" && (formulaEngine || proseEngine)) {
                QVector<QRect> localAreas = formulaRegions;
                for (auto &area : localAreas) area.translate(-result.offset);
                result.warnings = collectSpecialistReadings(page, input, localAreas,
                    formulaEngine.get(), proseEngine.get(), token);
                if (token.stop_requested()) result.outcome = std::unexpected(OcrError{{}, true});
            }
            if (result.outcome && !rawOutput && !splitOnly && engine->capabilities().supportsWordBoxes) {
                auto retries = collectSpellingFormulaReadings(page, input, token);
                if (!retries) result.outcome = std::unexpected(retries.error());
                else result.warnings += *retries;
            }
            if (result.outcome && !rawOutput && !splitOnly) {
                auto retries = collectFormulaTextReadings(page, input, options.language, token);
                if (!retries) result.outcome = std::unexpected(retries.error());
                else result.warnings += *retries;
            }
            if (result.outcome && !rawOutput && !splitOnly)
                applySymbolRules(page, input, symbolRules, token);
            if (result.outcome && !rawOutput && !splitOnly && unicodeFormulas)
                if (const int unsupported=core::setFormulaOutput(page,true);unsupported)
                    result.warnings.append(QStringLiteral("%1 formula(s) retained LaTeX because their Unicode conversion is unsupported.").arg(unsupported));
            if (token.stop_requested()) result.outcome = std::unexpected(OcrError{{}, true});
        }
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
                                        int pageNumber, const QRect &region)
{
    if (region.isEmpty()) { emit failed(tr("Select an image region first.")); return; }
    if (!engineCapabilities().supportsRegions) {
        emit failed(tr("The selected OCR engine does not support region recognition."));
        return;
    }
    startOcrImage(imagePath, {}, {.language = language, .pageNumber = pageNumber}, region, false);
}

void DocumentProcessor::recognizePages(const QVector<BatchPageOptions> &pages)
{
    if (isBusy() || pages.isEmpty() || documentSource.isEmpty()) return;
    const QString renderer = pageRenderCommand(documentSource, 1, {}).program;
    if (QStandardPaths::findExecutable(renderer).isEmpty()) {
        emit failed(tr("%1 is not installed or is not on PATH.").arg(renderer));
        return;
    }
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
    if (QFileInfo::exists(pageFile(number))) adaptRenderedPage(number);
    const auto command = pageRenderCommand(documentSource, number, pagePrefix(number), false, selectedRenderDpi(number));
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

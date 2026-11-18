#include "documentprocessor.h"

#include "imageprocessor.h"
#include "tesseractengine.h"
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
        if (finishedJob == Job::BatchRender) {
            prepareBatchImage(pageRenderCommand(documentSource, batchPages.at(batchIndex).number,
                temporaryDir.filePath(renderPrefix + QString::number(batchPages.at(batchIndex).number))).outputPath);
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
        } else if (finishedJob == Job::DocumentRender)
            emit pageRendered(requestedPage,
                pageRenderCommand(documentSource, requestedPage,
                    temporaryDir.filePath(renderPrefix + QString::number(requestedPage))).outputPath);
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
        if (job != Job::Ocr && job != Job::BatchOcr) return;
        OcrJobResult result = ocrWatcher.result();
        const bool batch = job == Job::BatchOcr;
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
            emit recognized(std::move(*result.outcome), result.offset, result.region);
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
    if (!isBusy() && replacement) engine = std::move(replacement);
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

void DocumentProcessor::renderDocumentPage(int pageNumber)
{
    if (isBusy() || documentSource.isEmpty() || pageNumber < 1) return;
    requestedPage = pageNumber;
    const QString outputPrefix = temporaryDir.filePath(renderPrefix + QString::number(pageNumber));
    const auto command = pageRenderCommand(documentSource, pageNumber, outputPrefix);
    const QString imagePath = command.outputPath;
    if (QFileInfo::exists(imagePath)) {
        emit pageRendered(pageNumber, imagePath);
        return;
    }
    start(Job::DocumentRender, command.program, command.arguments);
}

void DocumentProcessor::discardRenderedPage(Job finishedJob)
{
    if (finishedJob != Job::DocumentRender && finishedJob != Job::BatchRender) return;
    const int number = finishedJob == Job::BatchRender ? batchPages.value(batchIndex).number : requestedPage;
    QFile::remove(pageRenderCommand(documentSource, number,
        temporaryDir.filePath(renderPrefix + QString::number(number))).outputPath);
}

void DocumentProcessor::startOcrImage(const QString &path, const QImage &image,
                                      OcrOptions options, const QRect &region, bool batch)
{
    if ((!batch && isBusy()) || (batch && batchPages.isEmpty())) return;
    if (book && engine->id() == "tesseract") {
        if (!batch || batchIndex == 0 || vocabularyLanguage != options.language ||
            nextEvidencePage > options.pageNumber) {
            batchVocabulary = {};
            vocabularyLanguage = options.language;
            nextEvidencePage = 1;
        }
        // In a sparse project a high page number can have very few recognized predecessors.
        for (const auto &page : book->pages) {
            if (page.number >= nextEvidencePage && page.number < options.pageNumber &&
                page.language == options.language) batchVocabulary.addPage(page);
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
    job = batch ? Job::BatchOcr : Job::Ocr;
    cancellationRequested = false;
    ocrStop = std::stop_source{};
    const std::stop_token token = ocrStop.get_token();
    if (!wasBusy) emit busyChanged(true);
    ocrWatcher.setFuture(QtConcurrent::run([this, path, image, options, region, token, formulaRegions] {
        OcrJobResult result;
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
        result.outcome = engine->recognize(input, options, token);
        if (result.outcome) {
            core::OcrPage &page = *result.outcome;
            page.engineId = engine->id();
            if (page.number != options.pageNumber || page.width != input.width() ||
                page.height != input.height()) {
                result.outcome = std::unexpected(OcrError{
                    QStringLiteral("The OCR engine returned inconsistent page geometry.")});
                return result;
            }
            page.rebuildHierarchy();
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
            if (engine->id() == "tesseract" && (formulaEngine || proseEngine)) {
                QVector<QRect> localAreas = formulaRegions;
                for (auto &area : localAreas) area.translate(-result.offset);
                result.warnings = collectSpecialistReadings(page, input, localAreas,
                    formulaEngine.get(), proseEngine.get(), token);
                if (token.stop_requested()) result.outcome = std::unexpected(OcrError{{}, true});
            }
        }
        return result;
    }));
}

void DocumentProcessor::recognize(const QString &imagePath, const QString &language, int pageNumber)
{
    startOcrImage(imagePath, {}, {.language = language, .pageNumber = pageNumber}, {}, false);
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
    const QString outputPrefix = temporaryDir.filePath(renderPrefix + QString::number(number));
    const auto command = pageRenderCommand(documentSource, number, outputPrefix);
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
    if (job == Job::Ocr || job == Job::BatchOcr) {
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

#include "documentprocessor.h"

#include <QStandardPaths>
#include <QFileInfo>
#include <QRegularExpression>
#include <QUuid>
#include <QImage>
#include <QTimer>

#include "imageprocessor.h"

namespace yagf::processing {

DocumentProcessor::DocumentProcessor(QObject *parent) : QObject(parent)
{
    connect(&process, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus status) {
        const Job finishedJob = job;
        if (finishedJob == Job::None) return;
        if (cancellationRequested) {
            cancellationRequested = false;
            job = Job::None;
            batchPages.clear();
            emit busyChanged(false);
            emit cancelled();
            return;
        }
        if (status != QProcess::NormalExit || exitCode != 0) {
            const QString message = tr("Processing failed: %1").arg(QString::fromUtf8(process.readAllStandardError()));
            if (finishedJob == Job::BatchRender || finishedJob == Job::BatchOcr) finishBatchError(message);
            else { job = Job::None; emit busyChanged(false); emit failed(message); }
            return;
        }
        if (finishedJob == Job::BatchRender) {
            prepareBatchImage(temporaryDir.filePath(renderPrefix +
                QString::number(batchPages.at(batchIndex).number) + QStringLiteral(".png")));
            return;
        }
        if (finishedJob == Job::BatchOcr) {
            const BatchPageOptions completed = batchPages.at(batchIndex);
            emit batchPageRecognized(completed.number, process.readAllStandardOutput(),
                                     batchSourceSize, batchProcessedSize, completed.language);
            if (batchPages.isEmpty()) return;
            ++batchIndex;
            emit batchProgress(batchIndex, batchPages.size());
            QTimer::singleShot(0, this, &DocumentProcessor::startNextBatchPage);
            return;
        }
        job = Job::None;
        emit busyChanged(false);
        if (finishedJob == Job::PdfInfo) {
            const QString info = QString::fromUtf8(process.readAllStandardOutput());
            const QRegularExpression pagesPattern(QStringLiteral("(?:^|\\n)Pages:\\s*(\\d+)"));
            const auto match = pagesPattern.match(info);
            const int count = match.hasMatch() ? match.captured(1).toInt() : 0;
            if (count < 1) emit failed(tr("Could not read PDF page count."));
            else emit pdfOpened(count);
        } else if (finishedJob == Job::PdfRender)
            emit pageRendered(requestedPage, temporaryDir.filePath(renderPrefix + QString::number(requestedPage) + QStringLiteral(".png")));
        else if (finishedJob == Job::Ocr)
            emit recognized(process.readAllStandardOutput(), recognitionOffset, recognitionRegion);
    });
    connect(&process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            batchPages.clear();
            job = Job::None;
            emit busyChanged(false);
            emit failed(tr("Could not start %1.").arg(process.program()));
        }
    });
    connect(&thumbnailProcess, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus status) {
        if (nextThumbnail < 1 || nextThumbnail > thumbnailCount) return;
        const int completed = nextThumbnail++;
        const QString path = temporaryDir.filePath(renderPrefix + QStringLiteral("thumb-") +
                                                   QString::number(completed) + QStringLiteral(".png"));
        if (status == QProcess::NormalExit && exitCode == 0 && QFileInfo::exists(path))
            emit thumbnailReady(completed, path);
        startNextThumbnail();
    });
}

DocumentProcessor::~DocumentProcessor()
{
    thumbnailCount = 0;
    if (thumbnailProcess.state() != QProcess::NotRunning) {
        thumbnailProcess.kill();
        thumbnailProcess.waitForFinished(2000);
    }
    if (process.state() != QProcess::NotRunning) {
        process.kill();
        process.waitForFinished(2000);
    }
}

bool DocumentProcessor::isBusy() const { return job != Job::None; }

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

void DocumentProcessor::openPdf(const QString &path)
{
    resetSource();
    pdfSource = path;
    renderPrefix = QUuid::createUuid().toString(QUuid::WithoutBraces) + QLatin1Char('-');
    start(Job::PdfInfo, QStringLiteral("pdfinfo"), {path});
}

void DocumentProcessor::resetSource()
{
    thumbnailCount = 0;
    nextThumbnail = 0;
    if (thumbnailProcess.state() != QProcess::NotRunning) {
        thumbnailProcess.kill();
        thumbnailProcess.waitForFinished(2000);
    }
    pdfSource.clear();
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
    thumbnailProcess.start(QStringLiteral("pdftoppm"),
        {QStringLiteral("-f"), QString::number(nextThumbnail),
         QStringLiteral("-l"), QString::number(nextThumbnail),
         QStringLiteral("-singlefile"), QStringLiteral("-scale-to"), QStringLiteral("160"),
         QStringLiteral("-png"), pdfSource, output});
}

void DocumentProcessor::renderPdfPage(int pageNumber)
{
    if (isBusy() || pdfSource.isEmpty() || pageNumber < 1) return;
    requestedPage = pageNumber;
    const QString outputPrefix = temporaryDir.filePath(renderPrefix + QString::number(pageNumber));
    const QString imagePath = outputPrefix + QStringLiteral(".png");
    if (QFileInfo::exists(imagePath)) {
        emit pageRendered(pageNumber, imagePath);
        return;
    }
    start(Job::PdfRender, QStringLiteral("pdftoppm"),
          {QStringLiteral("-f"), QString::number(pageNumber), QStringLiteral("-l"), QString::number(pageNumber),
           QStringLiteral("-singlefile"), QStringLiteral("-r"), QStringLiteral("150"),
           QStringLiteral("-png"), pdfSource, outputPrefix});
}

void DocumentProcessor::recognize(const QString &imagePath, const QString &language)
{
    recognitionOffset = {};
    recognitionRegion = {};
    start(Job::Ocr, QStringLiteral("tesseract"),
          {imagePath, QStringLiteral("stdout"), QStringLiteral("-l"), language, QStringLiteral("tsv")});
}

void DocumentProcessor::recognizeRegion(const QString &imagePath, const QString &language, const QRect &region)
{
    if (isBusy()) return;
    const QImage source(imagePath);
    const QRect bounded = region.intersected(source.rect());
    if (source.isNull() || bounded.isEmpty()) {
        emit failed(tr("The selected region is outside the image."));
        return;
    }
    const QString cropPath = temporaryDir.filePath(QUuid::createUuid().toString(QUuid::WithoutBraces) +
                                                    QStringLiteral("-region.png"));
    if (!source.copy(bounded).save(cropPath)) {
        emit failed(tr("Could not prepare the selected region for OCR."));
        return;
    }
    recognitionOffset = bounded.topLeft();
    recognitionRegion = bounded;
    start(Job::Ocr, QStringLiteral("tesseract"),
          {cropPath, QStringLiteral("stdout"), QStringLiteral("-l"), language, QStringLiteral("tsv")});
}

void DocumentProcessor::recognizePages(const QVector<BatchPageOptions> &pages)
{
    if (isBusy() || pages.isEmpty() || pdfSource.isEmpty()) return;
    for (const QString &program : {QStringLiteral("pdftoppm"), QStringLiteral("tesseract")}) {
        if (QStandardPaths::findExecutable(program).isEmpty()) {
            emit failed(tr("%1 is not installed or is not on PATH.").arg(program));
            return;
        }
    }
    batchPages = pages;
    batchIndex = 0;
    job = Job::BatchRender;
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
    const QString imagePath = outputPrefix + QStringLiteral(".png");
    if (QFileInfo::exists(imagePath)) {
        prepareBatchImage(imagePath);
        return;
    }
    job = Job::BatchRender;
    process.start(QStringLiteral("pdftoppm"),
        {QStringLiteral("-f"), QString::number(number), QStringLiteral("-l"), QString::number(number),
         QStringLiteral("-singlefile"), QStringLiteral("-r"), QStringLiteral("150"),
         QStringLiteral("-png"), pdfSource, outputPrefix});
}

void DocumentProcessor::prepareBatchImage(const QString &path)
{
    const QImage source(path);
    if (source.isNull()) { finishBatchError(tr("Could not read rendered page %1.").arg(batchPages.at(batchIndex).number)); return; }
    const auto processed = ImageProcessor::apply(source, batchPages.at(batchIndex).preprocessing);
    if (!processed) { finishBatchError(processed.error()); return; }
    batchSourceSize = source.size();
    batchProcessedSize = processed->size();
    QString ocrPath = path;
    if (!batchPages.at(batchIndex).preprocessing.isIdentity()) {
        ocrPath = temporaryDir.filePath(renderPrefix + QStringLiteral("batch-") +
                                        QString::number(batchPages.at(batchIndex).number) + QStringLiteral(".png"));
        if (!processed->save(ocrPath, "PNG")) {
            finishBatchError(tr("Could not save prepared page %1.").arg(batchPages.at(batchIndex).number));
            return;
        }
    }
    job = Job::BatchOcr;
    process.start(QStringLiteral("tesseract"),
        {ocrPath, QStringLiteral("stdout"), QStringLiteral("-l"),
         batchPages.at(batchIndex).language, QStringLiteral("tsv")});
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
    if (isBusy()) {
        cancellationRequested = true;
        if (process.state() != QProcess::NotRunning) process.kill();
        else {
            cancellationRequested = false;
            batchPages.clear();
            job = Job::None;
            emit busyChanged(false);
            emit cancelled();
        }
    }
}

} // namespace yagf::processing

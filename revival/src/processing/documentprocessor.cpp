#include "documentprocessor.h"

#include <QStandardPaths>
#include <QFileInfo>
#include <QRegularExpression>
#include <QUuid>
#include <QImage>

DocumentProcessor::DocumentProcessor(QObject *parent) : QObject(parent)
{
    connect(&process, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus status) {
        const Job finishedJob = job;
        job = Job::None;
        emit busyChanged(false);
        if (cancellationRequested) {
            cancellationRequested = false;
            emit cancelled();
            return;
        }
        if (status != QProcess::NormalExit || exitCode != 0) {
            emit failed(tr("Processing failed: %1").arg(QString::fromUtf8(process.readAllStandardError())));
            return;
        }
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

void DocumentProcessor::cancel()
{
    if (isBusy()) {
        cancellationRequested = true;
        process.kill();
    }
}

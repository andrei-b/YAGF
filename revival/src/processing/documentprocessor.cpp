#include "documentprocessor.h"

#include <QStandardPaths>
#include <QFileInfo>
#include <QRegularExpression>
#include <QUuid>

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
            emit recognized(process.readAllStandardOutput());
    });
    connect(&process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            job = Job::None;
            emit busyChanged(false);
            emit failed(tr("Could not start %1.").arg(process.program()));
        }
    });
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
    pdfSource = path;
    renderPrefix = QUuid::createUuid().toString(QUuid::WithoutBraces) + QLatin1Char('-');
    start(Job::PdfInfo, QStringLiteral("pdfinfo"), {path});
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
    start(Job::Ocr, QStringLiteral("tesseract"),
          {imagePath, QStringLiteral("stdout"), QStringLiteral("-l"), language, QStringLiteral("tsv")});
}

void DocumentProcessor::cancel()
{
    if (isBusy()) {
        cancellationRequested = true;
        process.kill();
    }
}

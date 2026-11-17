#include "documentprocessor.h"

#include <QStandardPaths>

DocumentProcessor::DocumentProcessor(QObject *parent) : QObject(parent)
{
    connect(&process, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus status) {
        const Job finishedJob = job;
        job = Job::None;
        emit busyChanged(false);
        if (status != QProcess::NormalExit || exitCode != 0) {
            emit failed(tr("Processing failed: %1").arg(QString::fromUtf8(process.readAllStandardError())));
            return;
        }
        if (finishedJob == Job::Pdf)
            emit pageRendered(temporaryDir.filePath(QStringLiteral("page.png")));
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
    emit busyChanged(true);
    process.start(program, arguments);
}

void DocumentProcessor::renderFirstPdfPage(const QString &path)
{
    start(Job::Pdf, QStringLiteral("pdftoppm"),
          {QStringLiteral("-f"), QStringLiteral("1"), QStringLiteral("-l"), QStringLiteral("1"),
           QStringLiteral("-singlefile"), QStringLiteral("-r"), QStringLiteral("150"),
           QStringLiteral("-png"), path, temporaryDir.filePath(QStringLiteral("page"))});
}

void DocumentProcessor::recognize(const QString &imagePath, const QString &language)
{
    start(Job::Ocr, QStringLiteral("tesseract"),
          {imagePath, QStringLiteral("stdout"), QStringLiteral("-l"), language, QStringLiteral("tsv")});
}

void DocumentProcessor::cancel()
{
    if (isBusy()) process.kill();
}

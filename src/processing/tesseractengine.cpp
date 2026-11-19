#include "tesseractengine.h"

#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTemporaryDir>
#ifdef Q_OS_UNIX
#include <csignal>
#include <unistd.h>
#endif

namespace yagf::processing {

std::expected<core::OcrPage, OcrError> TesseractEngine::recognize(
    const QImage &image, const OcrOptions &options, std::stop_token stop)
{
    return recognize(image, options, stop, nullptr);
}

std::expected<core::OcrPage, OcrError> TesseractEngine::recognize(
    const QImage &image, const OcrOptions &options, std::stop_token stop, const std::atomic_bool *cancelled)
{
    runDetails = {};
    const auto interrupted = [&] { return stop.stop_requested() || (cancelled && cancelled->load()); };
    if (interrupted()) return std::unexpected(OcrError{{}, true});
    if (image.isNull() || options.language.trimmed().isEmpty() || options.pageNumber < 1)
        return std::unexpected(OcrError{QStringLiteral("Invalid OCR image or options.")});
    if (QStandardPaths::findExecutable(QStringLiteral("tesseract")).isEmpty())
        return std::unexpected(OcrError{QStringLiteral("tesseract is not installed or is not on PATH.")});
    QTemporaryDir temporary;
    if (!temporary.isValid())
        return std::unexpected(OcrError{QStringLiteral("Could not create OCR temporary files.")});
    const QString imagePath = temporary.filePath(QStringLiteral("input.png"));
    if (!image.save(imagePath, "PNG"))
        return std::unexpected(OcrError{QStringLiteral("Could not prepare the OCR image.")});
    if (interrupted()) return std::unexpected(OcrError{{}, true});
    QStringList arguments{imagePath, QStringLiteral("stdout"), QStringLiteral("-l"), options.language};
    if (!options.userWords.isEmpty()) {
        QFile vocabulary(temporary.filePath(QStringLiteral("book-words.txt")));
        const QByteArray data = options.userWords.join('\n').toUtf8() + '\n';
        if (!vocabulary.open(QIODevice::WriteOnly) || vocabulary.write(data) != data.size() || !vocabulary.flush())
            return std::unexpected(OcrError{QStringLiteral("Could not prepare the book vocabulary.")});
        arguments << QStringLiteral("--user-words") << vocabulary.fileName();
    }
    arguments << QStringLiteral("tsv");
    QProcess process;
    auto environment = QProcessEnvironment::systemEnvironment();
    // Avoid excessive OpenMP workers on document/region queues; honor user overrides.
    if (!environment.contains("OMP_THREAD_LIMIT")) environment.insert("OMP_THREAD_LIMIT", "2");
    process.setProcessEnvironment(environment);
    runDetails["omp_thread_limit"]=environment.value("OMP_THREAD_LIMIT");
    const QString memoryFile = temporary.filePath(QStringLiteral("peak-rss.txt"));
    const bool measureMemory=options.collectBenchmarkMetrics && QFile::exists(QStringLiteral("/usr/bin/time"));
    if(measureMemory) {
#ifdef Q_OS_UNIX
        // Cancel GNU time and its recognition child together.
        process.setChildProcessModifier([]{::setsid();});
#endif
        QStringList measured{"-f","%M","-o",memoryFile,"--",QStandardPaths::findExecutable(QStringLiteral("tesseract"))};
        measured+=arguments;process.start(QStringLiteral("/usr/bin/time"),measured);
    } else process.start(QStringLiteral("tesseract"), arguments);
    if (!process.waitForStarted(10'000))
        return std::unexpected(OcrError{QStringLiteral("Could not start tesseract: %1")
            .arg(process.errorString())});
    QElapsedTimer timer;
    timer.start();
    while (process.state() != QProcess::NotRunning) {
        if (interrupted() || timer.elapsed() > 300'000) {
#ifdef Q_OS_UNIX
            if(measureMemory && process.processId()>0)::kill(-process.processId(),SIGKILL);
#endif
            process.kill();
            process.waitForFinished(2000);
            if (interrupted()) return std::unexpected(OcrError{{}, true});
            return std::unexpected(OcrError{QStringLiteral("Tesseract timed out.")});
        }
        process.waitForFinished(100);
    }
    if (interrupted()) return std::unexpected(OcrError{{}, true});
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        return std::unexpected(OcrError{QStringLiteral("Tesseract failed: %1")
            .arg(QString::fromUtf8(process.readAllStandardError()).trimmed())});
    if(measureMemory) {
        QFile memory(memoryFile);
        if(memory.open(QIODevice::ReadOnly)) {
            bool ok=false;const auto kib=memory.readAll().trimmed().toLongLong(&ok);
            if(ok && kib>0){runDetails["peak_rss_bytes"]=kib*1024;runDetails["peak_rss_scope"]="Recognition child process";}
        }
    }
    return parseTsv(process.readAllStandardOutput(), image.size(), options);
}

std::expected<core::OcrPage, OcrError> TesseractEngine::parseTsv(
    const QByteArray &tsv, const QSize &imageSize, const OcrOptions &options)
{
    const QStringList rows = QString::fromUtf8(tsv).split(QLatin1Char('\n'));
    if (rows.isEmpty() || !rows.first().startsWith(QStringLiteral("level\t")))
        return std::unexpected(OcrError{QStringLiteral("Tesseract returned invalid TSV output.")});
    core::OcrPage page;
    page.id = QStringLiteral("page-%1").arg(options.pageNumber);
    page.number = options.pageNumber;
    page.width = page.sourceWidth = imageSize.width();
    page.height = page.sourceHeight = imageSize.height();
    page.engineId = QStringLiteral("tesseract");
    page.language = options.language;
    for (int i = 1; i < rows.size(); ++i) {
        const QStringList fields = rows.at(i).trimmed().split(QLatin1Char('\t'));
        if (fields.size() < 12 || fields.at(0) != QStringLiteral("5")) continue;
        bool ok[8] = {};
        core::OcrWord word;
        word.page = options.pageNumber;
        word.block = fields.at(2).toInt(&ok[0]);
        word.paragraph = fields.at(3).toInt(&ok[1]);
        word.line = fields.at(4).toInt(&ok[2]);
        const int x = fields.at(6).toInt(&ok[3]);
        const int y = fields.at(7).toInt(&ok[4]);
        const int width = fields.at(8).toInt(&ok[5]);
        const int height = fields.at(9).toInt(&ok[6]);
        word.confidence = fields.at(10).toDouble(&ok[7]);
        word.originalText = fields.mid(11).join(QLatin1Char('\t')).trimmed();
        word.text = word.originalText;
        bool valid = !word.text.isEmpty() && x >= 0 && y >= 0 && width > 0 && height > 0 &&
            static_cast<qint64>(x) + width <= imageSize.width() &&
            static_cast<qint64>(y) + height <= imageSize.height();
        for (bool value : ok) valid = valid && value;
        if (!valid) continue;
        word.bounds = QRect(x, y, width, height);
        page.words.append(std::move(word));
    }
    page.rebuildHierarchy();
    return page;
}

} // namespace yagf::processing

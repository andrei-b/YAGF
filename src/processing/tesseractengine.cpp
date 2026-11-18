#include "tesseractengine.h"

#include <QElapsedTimer>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

namespace yagf::processing {

std::expected<core::OcrPage, OcrError> TesseractEngine::recognize(
    const QImage &image, const OcrOptions &options, std::stop_token stop)
{
    if (stop.stop_requested()) return std::unexpected(OcrError{{}, true});
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
    if (stop.stop_requested()) return std::unexpected(OcrError{{}, true});
    QProcess process;
    process.start(QStringLiteral("tesseract"),
        {imagePath, QStringLiteral("stdout"), QStringLiteral("-l"),
         options.language, QStringLiteral("tsv")});
    if (!process.waitForStarted(10'000))
        return std::unexpected(OcrError{QStringLiteral("Could not start tesseract: %1")
            .arg(process.errorString())});
    QElapsedTimer timer;
    timer.start();
    while (process.state() != QProcess::NotRunning) {
        if (stop.stop_requested() || timer.elapsed() > 300'000) {
            process.kill();
            process.waitForFinished(2000);
            if (stop.stop_requested()) return std::unexpected(OcrError{{}, true});
            return std::unexpected(OcrError{QStringLiteral("Tesseract timed out.")});
        }
        process.waitForFinished(100);
    }
    if (stop.stop_requested()) return std::unexpected(OcrError{{}, true});
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        return std::unexpected(OcrError{QStringLiteral("Tesseract failed: %1")
            .arg(QString::fromUtf8(process.readAllStandardError()).trimmed())});
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

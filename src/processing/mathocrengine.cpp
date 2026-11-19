#include "mathocrengine.h"

#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

namespace yagf::processing {

QString MathOcrEngine::pythonPath()
{
    return QStandardPaths::locate(QStandardPaths::GenericDataLocation,
        QStringLiteral("yagf2/math-ocr/venv/bin/python"));
}

std::expected<core::OcrPage, OcrError> MathOcrEngine::recognize(
    const QImage &image, const OcrOptions &options, std::stop_token stop)
{
    return recognizeBoxes(image, options, stop, {});
}

std::expected<core::OcrPage, OcrError> MathOcrEngine::recognizeBoxes(
    const QImage &image, const OcrOptions &options, std::stop_token stop, const QList<QRect> &boxes)
{
    if (stop.stop_requested()) return std::unexpected(OcrError{{}, true});
    const QString python = pythonPath();
    if (!QFileInfo(python).isExecutable())
        return std::unexpected(OcrError{QStringLiteral("Local math OCR is not installed. See Local math OCR in the YAGF README.")});
    QTemporaryDir temporary;
    const QString input = temporary.filePath("page.png");
    const QString script = temporary.filePath("math_ocr.py");
    if (!temporary.isValid() || !image.save(input, "PNG") || !QFile::copy(":/math/math_ocr.py", script))
        return std::unexpected(OcrError{QStringLiteral("Could not prepare the local math OCR input.")});

    QProcess process;
    QStringList arguments{script, input};
    if (!boxes.isEmpty()) {
        QJsonArray crops;
        for (const auto &box : boxes)
            crops.append(QJsonArray{box.x(), box.y(), box.x() + box.width(), box.y() + box.height()});
        QFile file(temporary.filePath("boxes.json"));
        const auto data = QJsonDocument(crops).toJson(QJsonDocument::Compact);
        if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.flush())
            return std::unexpected(OcrError{QStringLiteral("Could not prepare equation crops.")});
        arguments.append(file.fileName());
    }
    process.start(python, arguments);
    if (!process.waitForStarted())
        return std::unexpected(OcrError{process.errorString()});
    QElapsedTimer timer;
    timer.start();
    while (!process.waitForFinished(100) && process.state() != QProcess::NotRunning) {
        if (stop.stop_requested() || timer.elapsed() > 300'000) {
            process.kill();
            process.waitForFinished();
            return std::unexpected(OcrError{QStringLiteral("Local math OCR timed out."), stop.stop_requested()});
        }
    }
    if (stop.stop_requested()) return std::unexpected(OcrError{{}, true});
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        return std::unexpected(OcrError{QStringLiteral("Local math OCR failed: %1")
            .arg(QString::fromUtf8(process.readAllStandardError()).trimmed().section('\n', -1))});
    const auto json = QJsonDocument::fromJson(process.readAllStandardOutput());
    if (!json.isArray()) return std::unexpected(OcrError{QStringLiteral("Invalid local math OCR output.")});

    core::OcrPage page;
    page.number = options.pageNumber;
    page.id = QStringLiteral("page-%1").arg(page.number);
    page.width = page.sourceWidth = image.width();
    page.height = page.sourceHeight = image.height();
    page.language = QStringLiteral("auto");
    page.engineId = id();
    for (const auto &item : json.array()) {
        const auto entry = item.toObject();
        const auto box = entry.value("box").toArray();
        const QString text = entry.value("text").toString().trimmed();
        const QRect bounds(box.at(0).toInt(-1), box.at(1).toInt(-1),
                           box.at(2).toInt(), box.at(3).toInt());
        if (box.size() != 4 || bounds.isEmpty() || !image.rect().contains(bounds) || text.isEmpty())
            return std::unexpected(OcrError{QStringLiteral("Invalid local math OCR reading.")});
        core::OcrWord word;
        word.page = page.number;
        word.block = page.words.size() + 1;
        word.line = word.paragraph = 1;
        word.layoutBlock = true;
        word.bounds = bounds;
        word.text = word.originalText = text;
        page.words.append(std::move(word));
    }
    page.rebuildHierarchy();
    for (auto &region : page.regions) region.type = QStringLiteral("equation");
    return page;
}

} // namespace yagf::processing

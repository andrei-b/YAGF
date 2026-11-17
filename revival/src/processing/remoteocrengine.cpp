#include "remoteocrengine.h"

#include <QBuffer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

#include <cmath>
#include <climits>

namespace yagf::processing {
namespace {
constexpr auto protocol = "yagf-ocr/1";
constexpr qsizetype maxPngBytes = 32 * 1024 * 1024;
constexpr qsizetype maxResponseBytes = 8 * 1024 * 1024;

std::expected<core::OcrPage, OcrError> parseResponse(
    const QByteArray &bytes, const QSize &imageSize, const OcrOptions &options,
    RemoteOcrEngine::Provider provider)
{
    if (bytes.size() > maxResponseBytes)
        return std::unexpected(OcrError{QStringLiteral("The OCR server response is too large.")});
    QJsonParseError parseError;
    const QJsonDocument json = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !json.isObject())
        return std::unexpected(OcrError{QStringLiteral("The OCR server returned invalid JSON.")});
    const QJsonObject root = json.object();
    const bool doctr = provider == RemoteOcrEngine::Provider::DocTr;
    const QString itemKey = doctr ? QStringLiteral("words") : QStringLiteral("lines");
    if (root.value(QStringLiteral("protocol")).toString() != QLatin1String(protocol) ||
        root.value(QStringLiteral("engine_id")).toString() !=
            (doctr ? QStringLiteral("doctr") : QStringLiteral("paddleocr")) ||
        root.value(QStringLiteral("width")).toInt(-1) != imageSize.width() ||
        root.value(QStringLiteral("height")).toInt(-1) != imageSize.height() ||
        !root.value(itemKey).isArray())
        return std::unexpected(OcrError{QStringLiteral("The OCR server response has an incompatible schema or image size.")});
    const QJsonArray items = root.value(itemKey).toArray();
    if (items.size() > 10000)
        return std::unexpected(OcrError{QStringLiteral("The OCR server returned too many text items.")});
    core::OcrPage page;
    page.id = QStringLiteral("page-%1").arg(options.pageNumber);
    page.number = options.pageNumber;
    page.width = page.sourceWidth = imageSize.width();
    page.height = page.sourceHeight = imageSize.height();
    page.engineId = doctr ? QStringLiteral("doctr-remote") : QStringLiteral("paddleocr-remote");
    page.language = root.value(QStringLiteral("language")).toString(options.language);
    int previousBlock = 0;
    int previousLine = 0;
    for (const QJsonValue &value : items) {
        if (!value.isObject())
            return std::unexpected(OcrError{QStringLiteral("The OCR server returned an invalid text item.")});
        const QJsonObject line = value.toObject();
        const QJsonArray box = line.value(QStringLiteral("box")).toArray();
        const QString text = line.value(QStringLiteral("text")).toString().trimmed();
        const QJsonValue score = line.value(QStringLiteral("confidence"));
        if (box.size() != 4 || text.isEmpty() || !score.isDouble() ||
            !std::isfinite(score.toDouble()) || score.toDouble() < 0 || score.toDouble() > 1)
            return std::unexpected(OcrError{QStringLiteral("The OCR server returned invalid text or confidence.")});
        int geometry[4];
        for (int i = 0; i < 4; ++i) {
            if (!box.at(i).isDouble() || !std::isfinite(box.at(i).toDouble()) ||
                box.at(i).toDouble() != std::floor(box.at(i).toDouble()) ||
                box.at(i).toDouble() < 0 || box.at(i).toDouble() > INT_MAX)
                return std::unexpected(OcrError{QStringLiteral("The OCR server returned invalid text geometry.")});
            geometry[i] = box.at(i).toInt();
        }
        const QRect bounds(geometry[0], geometry[1], geometry[2], geometry[3]);
        if (bounds.isEmpty() || !QRect(QPoint(0, 0), imageSize).contains(bounds))
            return std::unexpected(OcrError{QStringLiteral("The OCR server returned out-of-bounds text geometry.")});
        core::OcrWord word;
        word.page = options.pageNumber;
        word.block = doctr ? line.value(QStringLiteral("block")).toInt(-1) : 1;
        word.paragraph = 1;
        word.line = doctr ? line.value(QStringLiteral("line")).toInt(-1) : page.words.size() + 1;
        if (word.block < 1 || word.line < 1 ||
            (doctr && (word.block < previousBlock ||
                       (word.block == previousBlock && word.line < previousLine))))
            return std::unexpected(OcrError{QStringLiteral("The OCR server returned invalid reading order.")});
        previousBlock = word.block;
        previousLine = word.line;
        word.bounds = bounds;
        word.originalText = word.text = text;
        word.confidence = score.toDouble() * 100.0;
        page.words.append(std::move(word));
    }
    page.rebuildHierarchy();
    return page;
}
}

RemoteOcrEngine::RemoteOcrEngine(QUrl server, QString bearerToken, Provider provider)
    : server(std::move(server)), bearerToken(std::move(bearerToken)), provider(provider) {}

QString RemoteOcrEngine::id() const
{
    return provider == Provider::DocTr ? QStringLiteral("doctr-remote") : QStringLiteral("paddleocr-remote");
}

EngineCapabilities RemoteOcrEngine::capabilities() const
{
    return provider == Provider::DocTr
        ? EngineCapabilities{.supportsRegions = true, .supportsWordBoxes = true,
                             .supportsConfidence = true, .supportsLayout = true}
        : EngineCapabilities{.supportsRegions = true, .supportsConfidence = true};
}

std::expected<core::OcrPage, OcrError> RemoteOcrEngine::recognize(
    const QImage &image, const OcrOptions &options, std::stop_token stop)
{
    if (stop.stop_requested()) return std::unexpected(OcrError{{}, true});
    if (image.isNull() || options.pageNumber < 1 ||
        (server.scheme() != QStringLiteral("http") && server.scheme() != QStringLiteral("https")) ||
        server.host().isEmpty() || !server.userName().isEmpty() || !server.password().isEmpty())
        return std::unexpected(OcrError{QStringLiteral("Invalid remote OCR server URL or image.")});
    QByteArray png;
    QBuffer buffer(&png);
    if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG"))
        return std::unexpected(OcrError{QStringLiteral("Could not encode the OCR image.")});
    if (png.size() > maxPngBytes)
        return std::unexpected(OcrError{QStringLiteral("The image is too large for remote OCR.")});
    QJsonObject payload{{"protocol", QLatin1String(protocol)},
                        {"image_png", QString::fromLatin1(png.toBase64())},
                        {"language", options.language},
                        {"page_number", options.pageNumber}};
    QUrl url = server;
    url.setPath(QStringLiteral("/v1/recognize"));
    url.setQuery(QString());
    url.setFragment(QString());
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    if (!bearerToken.isEmpty())
        request.setRawHeader("Authorization", "Bearer " + bearerToken.toUtf8());
    QNetworkAccessManager manager;
    QNetworkReply *reply = manager.post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    reply->setReadBufferSize(maxResponseBytes + 1);
    QEventLoop loop;
    bool timedOut = false;
    QTimer poll;
    poll.setInterval(50);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (stop.stop_requested()) reply->abort();
    });
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &loop, [&] {
        timedOut = true;
        reply->abort();
    });
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::readyRead, &loop, [&] {
        if (reply->bytesAvailable() > maxResponseBytes) reply->abort();
    });
    poll.start();
    deadline.start(180'000);
    if (!reply->isFinished()) loop.exec();
    poll.stop();
    deadline.stop();
    if (stop.stop_requested()) return std::unexpected(OcrError{{}, true});
    if (timedOut) return std::unexpected(OcrError{QStringLiteral("Remote OCR timed out.")});
    const QByteArray response = reply->readAll();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->error() != QNetworkReply::NoError || status != 200) {
        const QJsonObject error = QJsonDocument::fromJson(response).object();
        const QString message = error.value(QStringLiteral("error")).toString();
        return std::unexpected(OcrError{message.isEmpty()
            ? QStringLiteral("Remote OCR failed: %1").arg(reply->errorString()) : message});
    }
    return parseResponse(response, image.size(), options, provider);
}

} // namespace yagf::processing

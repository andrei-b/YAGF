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
#include <algorithm>
#include <limits>

namespace yagf::processing {
namespace {
constexpr auto protocol = "yagf-ocr/1";
constexpr qsizetype maxPngBytes = 32 * 1024 * 1024;
constexpr qsizetype maxResponseBytes = 8 * 1024 * 1024;

// Prefer a nearby low-ink line over cutting letters exactly at the midpoint.
// Measuring relative to each line's paper level tolerates a photographed gutter.
int splitBoundary(const QImage &image, bool vertical)
{
    const QImage gray = image.convertToFormat(QImage::Format_Grayscale8);
    const int length = vertical ? gray.width() : gray.height();
    const int cross = vertical ? gray.height() : gray.width();
    int best = length / 2;
    double bestScore = std::numeric_limits<double>::max();
    const int step = std::max(1, cross / 800);
    for (int position = length * 35 / 100; position <= length * 65 / 100; ++position) {
        double ink = 0;
        int total = 0;
        for (int delta = -1; delta <= 1; ++delta) {
            int sum = 0, count = 0;
            for (int k = cross / 20; k < cross * 19 / 20; k += step) {
                sum += vertical ? gray.constScanLine(k)[position + delta] : gray.constScanLine(position + delta)[k];
                ++count;
            }
            const double threshold = count ? double(sum) / count - 18 : 0;
            for (int k = cross / 20; k < cross * 19 / 20; k += step) {
                if ((vertical ? gray.constScanLine(k)[position + delta] : gray.constScanLine(position + delta)[k]) < threshold) ++ink;
                ++total;
            }
        }
        const double score = ink / std::max(1, total) + 0.015 * std::abs(position - length / 2.0) / length;
        if (score < bestScore) { best = position; bestScore = score; }
    }
    return best;
}

QString providerName(RemoteOcrEngine::Provider provider)
{
    switch (provider) {
    case RemoteOcrEngine::Provider::PaddleOcr: return QStringLiteral("paddleocr");
    case RemoteOcrEngine::Provider::DocTr: return QStringLiteral("doctr");
    case RemoteOcrEngine::Provider::RapidOcr: return QStringLiteral("rapidocr");
    case RemoteOcrEngine::Provider::Surya: return QStringLiteral("surya");
    case RemoteOcrEngine::Provider::Kraken: return QStringLiteral("kraken");
    case RemoteOcrEngine::Provider::Native: return QStringLiteral("native");
    case RemoteOcrEngine::Provider::PaddleVl: return QStringLiteral("paddleocr-vl");
    }
    return {};
}

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
    const bool orderedWords = provider == RemoteOcrEngine::Provider::DocTr ||
                              provider == RemoteOcrEngine::Provider::Kraken || provider == RemoteOcrEngine::Provider::Native;
    const bool structured = orderedWords || provider == RemoteOcrEngine::Provider::Surya;
    const QString itemKey = orderedWords ? QStringLiteral("words") : QStringLiteral("lines");
    if (root.value(QStringLiteral("protocol")).toString() != QLatin1String(protocol) ||
        root.value(QStringLiteral("engine_id")).toString() != providerName(provider) ||
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
    page.engineId = providerName(provider) + QStringLiteral("-remote");
    page.language = root.value(QStringLiteral("language")).toString(options.language);
    int previousBlock = 0;
    int previousLine = 0;
    for (const auto &value : items) {
        if (!value.isObject())
            return std::unexpected(OcrError{QStringLiteral("The OCR server returned an invalid text item.")});
        const QJsonObject line = value.toObject();
        const QJsonArray box = line.value(QStringLiteral("box")).toArray();
        const QString rawText = line.value(QStringLiteral("text")).toString();
        const QString text = provider == RemoteOcrEngine::Provider::Surya &&
            line.value("type").toString() == "table" ? rawText : rawText.trimmed();
        const QJsonValue score = line.value(QStringLiteral("confidence"));
        if (line.contains(QStringLiteral("layout_block")) && !line.value(QStringLiteral("layout_block")).isBool())
            return std::unexpected(OcrError{QStringLiteral("The OCR server returned an invalid layout block flag.")});
        const bool paddleBlock = provider == RemoteOcrEngine::Provider::PaddleOcr &&
                                 line.value(QStringLiteral("layout_block")).toBool();
        const bool unknownScore = (provider == RemoteOcrEngine::Provider::Surya || paddleBlock) && score.isNull();
        if (box.size() != 4 || text.isEmpty() || (!unknownScore && (!score.isDouble() ||
            !std::isfinite(score.toDouble()) || score.toDouble() < 0 || score.toDouble() > 1)))
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
        word.block = structured ? line.value(QStringLiteral("block")).toInt(-1) : 1;
        word.paragraph = 1;
        word.line = structured ? line.value(QStringLiteral("line")).toInt(-1) : static_cast<int>(page.words.size()) + 1;
        if (word.block < 1 || word.line < 1 ||
            (structured && (word.block < previousBlock ||
                            (word.block == previousBlock && word.line < previousLine))))
            return std::unexpected(OcrError{QStringLiteral("The OCR server returned invalid reading order.")});
        previousBlock = word.block;
        previousLine = word.line;
        word.layoutBlock = provider == RemoteOcrEngine::Provider::Surya || paddleBlock ||
                           (provider == RemoteOcrEngine::Provider::Native && line.value("layout_block").toBool());
        word.bounds = bounds;
        word.originalText = word.text = text;
        word.confidence = unknownScore ? -1 : score.toDouble() * 100.0;
        page.words.append(std::move(word));
    }
    page.rebuildHierarchy();
    if (provider == RemoteOcrEngine::Provider::Surya) {
        if (page.regions.size() != items.size())
            return std::unexpected(OcrError{QStringLiteral("Surya returned inconsistent layout blocks.")});
        for (int i = 0; i < items.size(); ++i) {
            const QString type = items.at(i).toObject().value(QStringLiteral("type")).toString();
            if (type.isEmpty())
                return std::unexpected(OcrError{QStringLiteral("Surya returned an invalid region type.")});
            page.regions[i].type = type;
            const auto layout = items.at(i).toObject().value("table_layout");
            if (!layout.isUndefined()) {
                if (type != "table" || !core::OcrRegion::validTableLayout(layout))
                    return std::unexpected(OcrError{QStringLiteral("Surya returned an invalid table layout.")});
                page.regions[i].tableLayout = layout.toArray();
            }
        }
    }
    return page;
}
}

RemoteOcrEngine::RemoteOcrEngine(QUrl server, QString bearerToken, Provider provider, QString model, bool formulas)
    : server(std::move(server)), bearerToken(std::move(bearerToken)), provider(provider), model(std::move(model)), formulas(formulas) {}

QString RemoteOcrEngine::id() const
{
    return providerName(provider) + QStringLiteral("-remote");
}

EngineCapabilities RemoteOcrEngine::capabilities() const
{
    switch (provider) {
    case Provider::DocTr:
    case Provider::Kraken:
    case Provider::Native:
        return {.supportsRegions = true, .supportsWordBoxes = true,
                .supportsConfidence = true, .supportsLayout = true};
    case Provider::Surya:
        return {.supportsRegions = true, .supportsConfidence = true, .supportsLayout = true, .supportsFormulas = true};
    case Provider::PaddleVl:
        return {.supportsRegions = true, .supportsLayout = true, .supportsFormulas = true};
    case Provider::PaddleOcr:
    case Provider::RapidOcr:
        return {.supportsRegions = true, .supportsConfidence = true, .supportsFormulas = formulas};
    }
    return {};
}

std::expected<core::OcrPage, OcrError> RemoteOcrEngine::recognize(
    const QImage &image, const OcrOptions &options, std::stop_token stop)
{
    runDetails = {};
    if(options.collectBenchmarkMetrics)runDetails["inference_ms_scope"]="Successful responses only; client latency includes retries and failures";
    return recognizeSection(image, options, stop, 0);
}

std::expected<core::OcrPage, OcrError> RemoteOcrEngine::recognizeSection(
    const QImage &image, const OcrOptions &options, std::stop_token stop, int depth)
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
    if (options.formula) payload.insert("task", "formula");
    QUrl url = server;
    QString basePath=server.path(); while (basePath.endsWith('/')) basePath.chop(1);
    url.setPath(basePath+QStringLiteral("/v1/recognize"));
    if (provider == Provider::PaddleVl) {
        if (model.isEmpty() || model.size() > 256)
            return std::unexpected(OcrError{QStringLiteral("Choose a PaddleOCR-VL model using OCR discovery.")});
        url.setPath(basePath + (basePath.endsWith("/v1") ? "/chat/completions" : "/v1/chat/completions"));
        payload = {{"model", model}, {"temperature", 0}, {"stream", false}, {"max_tokens", 4096},
            {"messages", QJsonArray{QJsonObject{{"role", "user"}, {"content", QJsonArray{
                QJsonObject{{"type", "image_url"}, {"image_url", QJsonObject{{"url", "data:image/png;base64,"+QString::fromLatin1(png.toBase64())}}}},
                QJsonObject{{"type", "text"}, {"text", options.formula ? "Formula Recognition:" : "OCR:"}}
            }}}}}};
    }
    url.setQuery(QString());
    url.setFragment(QString());
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
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
    deadline.start(std::clamp(options.timeoutMs, 1, 180000));
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
        if (status == 400 && error.value("code") == "transcription_truncated" &&
            provider == Provider::PaddleOcr && !options.formula && depth < 2 && std::min(image.width(), image.height()) >= 96) {
            const bool vertical = image.width() > image.height() * 1.2;
            const int cut = splitBoundary(image, vertical);
            const QVector<QRect> sections = vertical
                ? QVector<QRect>{{0,0,cut,image.height()}, {cut,0,image.width()-cut,image.height()}}
                : QVector<QRect>{{0,0,image.width(),cut}, {0,cut,image.width(),image.height()-cut}};
            core::OcrPage page;
            page.id = QStringLiteral("page-%1").arg(options.pageNumber); page.number = options.pageNumber;
            page.engineId = id(); page.language = options.language;
            page.width = page.sourceWidth = image.width(); page.height = page.sourceHeight = image.height();
            int nextBlock = 1;
            for (const auto &section : sections) {
                auto part = recognizeSection(image.copy(section), options, stop, depth + 1);
                if (!part) return std::unexpected(part.error()); // Never publish only half a page.
                QHash<int,int> blocks;
                QHash<QString,QString> regions;
                for (auto word : part->words) {
                    if (!blocks.contains(word.block)) blocks.insert(word.block, nextBlock++);
                    regions.insert(QStringLiteral("page-%1/region-%2").arg(options.pageNumber).arg(word.block),
                                   QStringLiteral("page-%1/region-%2").arg(options.pageNumber).arg(blocks[word.block]));
                    word.block = blocks[word.block]; word.id.clear(); word.sourceWordIds.clear();
                    word.bounds.translate(section.topLeft());
                    for (auto &box : word.sourceBoxes) box.translate(section.topLeft());
                    page.words.append(word);
                }
                for (auto region : part->regions) {
                    region.id = regions.value(region.id); region.bounds.translate(section.topLeft());
                    if (!region.id.isEmpty()) page.regions.append(region);
                }
            }
            page.rebuildHierarchy();
            return page;
        }
        return std::unexpected(OcrError{message.isEmpty()
            ? QStringLiteral("Remote OCR failed: %1").arg(reply->errorString()) : message});
    }
    if (provider == Provider::PaddleVl) {
        const auto choices = QJsonDocument::fromJson(response).object().value("choices").toArray();
        const auto choice = choices.isEmpty() ? QJsonObject{} : choices.first().toObject();
        const QString text = choice.value("message").toObject().value("content").toString().trimmed();
        if (response.size() > maxResponseBytes || choice.value("finish_reason") != "stop" || text.isEmpty() || text.size() > 65536)
            return std::unexpected(OcrError{QStringLiteral("The formula/OCR server returned an empty, truncated or invalid transcription.")});
        core::OcrPage page;
        page.id = QStringLiteral("page-%1").arg(options.pageNumber); page.number = options.pageNumber;
        page.width = page.sourceWidth = image.width(); page.height = page.sourceHeight = image.height();
        page.engineId = id(); page.language = "auto";
        core::OcrWord word;
        word.page = page.number; word.block = word.paragraph = word.line = 1;
        word.bounds = image.rect(); word.text = word.originalText = text;
        word.layoutBlock = true; word.confidence = -1; word.readingEngine = id();
        page.words.append(std::move(word)); page.rebuildHierarchy();
        if (options.formula) page.regions.first().type = "equation";
        return page;
    }
    auto parsed=parseResponse(response, image.size(), options, provider);
    if(parsed && options.collectBenchmarkMetrics) {
        const auto root=QJsonDocument::fromJson(response).object();
        const auto details=root.value("run_details").toObject();
        for(const auto &key:{"inference_ms","queue_ms","peak_rss_bytes"}) {
            const auto v=details.value(QLatin1String(key));
            if(v.isDouble() && std::isfinite(v.toDouble()) && v.toDouble()>=0) {
                const double prior=runDetails.value(QLatin1String(key)).toDouble();
                runDetails[QLatin1String(key)]=QString::fromLatin1(key)=="peak_rss_bytes"?std::max(prior,v.toDouble()):prior+v.toDouble();
            }
        }
        for(const auto &key:{"engine","model","version","device","architecture","jetpack","peak_rss_scope","memory_note"}) {
            const auto v=details.value(QLatin1String(key));
            if(v.isString() && v.toString().size()<=1024)runDetails[QLatin1String(key)]=v;
        }
    }
    return parsed;
}

} // namespace yagf::processing

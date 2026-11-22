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
    case RemoteOcrEngine::Provider::Generic: return {};
    }
    return {};
}

std::expected<RecognitionResult, OcrError> parseResponse(
    const QByteArray &bytes, const QSize &imageSize, const OcrOptions &options,
    RemoteOcrEngine::Provider provider, const QString &engineName)
{
    if (bytes.size() > maxResponseBytes)
        return std::unexpected(OcrError{QStringLiteral("The OCR server response is too large.")});
    QJsonParseError parseError;
    const QJsonDocument json = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !json.isObject())
        return std::unexpected(OcrError{QStringLiteral("The OCR server returned invalid JSON.")});
    const QJsonObject root = json.object();
    const bool orderedWords = provider == RemoteOcrEngine::Provider::DocTr ||
                              provider == RemoteOcrEngine::Provider::Kraken || provider == RemoteOcrEngine::Provider::Native ||
                              (provider == RemoteOcrEngine::Provider::Generic && root["words"].isArray());
    const bool structured = orderedWords || provider == RemoteOcrEngine::Provider::Surya;
    const QString itemKey = orderedWords ? QStringLiteral("words") : QStringLiteral("lines");
    if (root.value(QStringLiteral("protocol")).toString() != QLatin1String(protocol) ||
        root.value(QStringLiteral("engine_id")).toString() != engineName ||
        root.value(QStringLiteral("width")).toInt(-1) != imageSize.width() ||
        root.value(QStringLiteral("height")).toInt(-1) != imageSize.height())
        return std::unexpected(OcrError{QStringLiteral("The OCR server response has an incompatible schema or image size.")});
    if (root.contains("recognition")) {
        if (!root["recognition"].isObject()) return std::unexpected(OcrError{"Invalid native recognition result."});
        auto result = core::readRecognitionResult(root["recognition"].toObject());
        if (!result || result->provenance.imageSize != imageSize)
            return std::unexpected(OcrError{result ? QStringLiteral("Incompatible recognition image size.") : result.error()});
        result->engineId = engineName+"-remote";
        result->provenance.pageNumber = options.pageNumber;
        return *result;
    }
    if (!root.value(itemKey).isArray()) {
        if (!root["markdown"].isString() || root["markdown"].toString().trimmed().isEmpty())
            return std::unexpected(OcrError{"The OCR server response has an incompatible schema."});
        RecognitionResult result; result.engineId = engineName+"-remote";
        result.modelVersion = root.value("model_version").toString();
        result.provenance = {.model=root.value("model").toString(), .task=options.formula ? "formula" : "ocr",
            .language=root.value("language").toString(options.language), .pageNumber=options.pageNumber, .imageSize=imageSize};
        result.markdown = root["markdown"].toString(); result.structuredOutput = root;
        return result;
    }
    const QJsonArray items = root.value(itemKey).toArray();
    if (items.size() > 10000)
        return std::unexpected(OcrError{QStringLiteral("The OCR server returned too many text items.")});
    RecognitionResult result;
    result.engineId = engineName + QStringLiteral("-remote");
    result.modelVersion = root.value("model_version").toString();
    result.provenance = {.model=root.value("model").toString(), .task=options.formula ? "formula" : "ocr",
        .language=root.value("language").toString(options.language), .pageNumber=options.pageNumber, .imageSize=imageSize};
    result.markdown = root.value("markdown").toString();
    result.structuredOutput = root;
    int previousBlock = 0, previousLine = 0;
    for (int index = 0; index < items.size(); ++index) {
        const auto value = items[index];
        if (!value.isObject()) return std::unexpected(OcrError{QStringLiteral("The OCR server returned an invalid text item.")});
        const auto item = value.toObject();
        const auto box = item.value("box").toArray();
        const QString rawText = item.value("text").toString();
        const QString type = provider == RemoteOcrEngine::Provider::Surya ? item.value("type").toString() : QStringLiteral("text");
        const QString text = type == "table" ? rawText : rawText.trimmed();
        const auto score = item.value("confidence");
        if (item.contains("layout_block") && !item.value("layout_block").isBool())
            return std::unexpected(OcrError{QStringLiteral("The OCR server returned an invalid layout block flag.")});
        const bool layoutBlock = provider == RemoteOcrEngine::Provider::Surya || item.value("layout_block").toBool();
        const bool unknownScore = (provider == RemoteOcrEngine::Provider::Surya ||
            (provider == RemoteOcrEngine::Provider::PaddleOcr && layoutBlock)) && score.isNull();
        if (box.size() != 4 || text.isEmpty() || type.isEmpty() || (!unknownScore && (!score.isDouble() ||
            !std::isfinite(score.toDouble()) || score.toDouble() < 0 || score.toDouble() > 1)))
            return std::unexpected(OcrError{QStringLiteral("The OCR server returned invalid text or confidence.")});
        int geometry[4];
        for (int i = 0; i < 4; ++i) {
            if (!box[i].isDouble() || !std::isfinite(box[i].toDouble()) || box[i].toDouble() != std::floor(box[i].toDouble()) ||
                box[i].toDouble() < 0 || box[i].toDouble() > INT_MAX)
                return std::unexpected(OcrError{QStringLiteral("The OCR server returned invalid text geometry.")});
            geometry[i] = box[i].toInt();
        }
        // Check sums before constructing QRect to avoid overflowing its edges.
        if (qint64(geometry[0])+geometry[2] > imageSize.width() || qint64(geometry[1])+geometry[3] > imageSize.height() ||
            geometry[2] < 1 || geometry[3] < 1)
            return std::unexpected(OcrError{QStringLiteral("The OCR server returned out-of-bounds text geometry.")});
        const QRect bounds(geometry[0], geometry[1], geometry[2], geometry[3]);
        const int block = structured ? item.value("block").toInt(-1) : 1;
        const int line = structured ? item.value("line").toInt(-1) : index+1;
        if (block < 1 || line < 1 || (structured && (block < previousBlock ||
            (block == previousBlock && (line < previousLine || provider == RemoteOcrEngine::Provider::Surya)))))
            return std::unexpected(OcrError{QStringLiteral("The OCR server returned invalid reading order.")});
        previousBlock = block; previousLine = line;
        core::RecognitionRegion base; base.bounds = bounds; base.order = index; base.confidence = unknownScore ? -1 : score.toDouble()*100;
        if (item.contains("geometry_source") && item["geometry_source"] != "input_extent" && item["geometry_source"] != "model")
            return std::unexpected(OcrError{"Invalid OCR geometry source."});
        // Older VL bridges already identified page-wide, unknown-confidence
        // blocks but did not label the input extent explicitly.
        if (item["geometry_source"] == "input_extent" || (!item.contains("geometry_source") &&
            provider == RemoteOcrEngine::Provider::PaddleOcr && layoutBlock && unknownScore && bounds == QRect(QPoint(), imageSize)))
            { base.bounds.reset(); base.inputArea = bounds; }
        const auto layout = item.value("table_layout");
        if (!layout.isUndefined() && (type != "table" || !core::OcrRegion::validTableLayout(layout)))
            return std::unexpected(OcrError{QStringLiteral("The OCR server returned an invalid table layout.")});
        if (type == "table") {
            core::TableRegion table; static_cast<core::RecognitionRegion &>(table) = base; table.text = text;
            if (!layout.isUndefined()) table.structure["table_layout"] = layout;
            result.tables.append(table);
        } else if (options.formula || type == "equation" || type == "formula") {
            core::FormulaRegion formula; static_cast<core::RecognitionRegion &>(formula) = base; formula.latex = text;
            result.formulas.append(formula);
        } else if (type == "figure" || type == "picture") {
            core::FigureRegion figure; static_cast<core::RecognitionRegion &>(figure) = base;
            figure.caption = text; figure.type = type; result.figures.append(figure);
        } else {
            core::TextRegion region; static_cast<core::RecognitionRegion &>(region) = base;
            region.text = text; region.type = type; region.block = block; region.paragraph = 1; region.line = line;
            region.granularity = layoutBlock ? core::TextGranularity::Block :
                orderedWords ? core::TextGranularity::Word : core::TextGranularity::Line;
            result.text.append(region);
        }
    }
    return result;
}
}

RemoteOcrEngine::RemoteOcrEngine(QUrl server, QString bearerToken, Provider provider, QString model, bool formulas,
    std::optional<ModelProfile> profile,QString engineId)
    : server(std::move(server)), bearerToken(std::move(bearerToken)), provider(provider), model(std::move(model)), formulas(formulas),
      profile(std::move(profile)),engineName(provider==Provider::Generic?std::move(engineId):providerName(provider)) {}

QString RemoteOcrEngine::id() const
{
    return engineName + QStringLiteral("-remote");
}

EngineCapabilities RemoteOcrEngine::capabilities() const
{
    if(profile)return {.supportsRegions=true,.supportsWordBoxes=profile->geometry=="word",
        .supportsLayout=profile->tasks.contains("layout"),.supportsTables=profile->tasks.contains("table"),
        .supportsFormulas=profile->tasks.contains("formula")};
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
    case Provider::Generic:
        return {};
    }
    return {};
}

std::expected<RecognitionResult, OcrError> RemoteOcrEngine::recognizeResult(
    const QImage &image, const OcrOptions &options, std::stop_token stop)
{
    runDetails = {}; responses={};
    if(stop.stop_requested())return std::unexpected(OcrError{{},true});
    auto requestOptions=options;
    if(!options.task.isEmpty()) {
        if(options.formula && canonicalModelTask(options.task)!="formula")
            return std::unexpected(OcrError{QStringLiteral("Conflicting formula and recognition task options.")});
        requestOptions.formula=canonicalModelTask(options.task)=="formula";
        if(provider==Provider::PaddleVl && canonicalModelTask(options.task)!="text" && canonicalModelTask(options.task)!="formula")
            return std::unexpected(OcrError{QStringLiteral("This chat adapter supports text and formula tasks only.")});
    }
    if(profile) {
        ModelRequirements requirements;requirements.tasks={options.task.isEmpty()?(options.formula?QStringLiteral("formula"):QStringLiteral("text")):canonicalModelTask(options.task)};
        requirements.format=options.outputFormat;
        if(profile->maxImagePixels)requirements.imageSize=image.size();
        // A missing list means unknown. An advertised list is authoritative.
        if(!profile->languages.isEmpty())requirements.language=options.language;
        const auto errors=profile->incompatibilities(requirements);
        if(!errors.isEmpty())return std::unexpected(OcrError{errors.join(' ')});
    }
    if(options.collectBenchmarkMetrics)runDetails["inference_ms_scope"]="Successful responses only; client latency includes retries and failures";
    auto result = recognizeSection(image, requestOptions, stop, 0);
    if (!result) return result;
    if(result->provenance.model.isEmpty())result->provenance.model=model;
    if(!options.task.isEmpty())result->provenance.task=canonicalModelTask(options.task);
    if (auto valid = core::validateRecognitionResult(*result); !valid)
        return std::unexpected(OcrError{valid.error()});
    return result;
}

std::expected<RecognitionResult, OcrError> RemoteOcrEngine::recognizeSection(
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
    if(!options.task.isEmpty())payload.insert("task",canonicalModelTask(options.task)=="text"?QStringLiteral("ocr"):options.task);
    if(!options.outputFormat.isEmpty())payload.insert("output_format",options.outputFormat);
    if(profile)payload.insert("model",model);
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
    bool oversized = false;
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
        if (reply->bytesAvailable() > maxResponseBytes) {
            oversized = true;
            reply->abort();
        }
    });
    poll.start();
    deadline.start(std::clamp(options.timeoutMs, 1, 180000));
    if (!reply->isFinished()) loop.exec();
    poll.stop();
    deadline.stop();
    const QByteArray response = reply->readAll();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if(options.history) {
        const auto json=QJsonDocument::fromJson(response);
        QJsonObject attempt{{"http_status",status},{"depth",depth},{"input_sha256",core::sha256(png)},
            {"input_size",QJsonArray{image.width(),image.height()}},
            {"processed_offset",QJsonArray{options.historyOffset.x(),options.historyOffset.y()}},
            {"source_to_input",options.historyCoordinates.toJson()}};
        if(json.isObject())attempt["response"]=json.object();
        else if(json.isArray())attempt["response"]=json.array();
        else attempt["raw_response_base64"]=QString::fromLatin1(response.toBase64());
        responses.append(attempt);
    }
    if (stop.stop_requested()) return std::unexpected(OcrError{{}, true});
    if (timedOut) return std::unexpected(OcrError{QStringLiteral("Remote OCR timed out.")});
    if (oversized)
        return std::unexpected(OcrError{QStringLiteral("Remote OCR response exceeded the %1 MiB limit.")
                                            .arg(qlonglong(maxResponseBytes / (1024 * 1024)))});
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
            RecognitionResult result;
            result.engineId = id();
            result.provenance = {.model=model, .task="ocr", .language=options.language,
                .pageNumber=options.pageNumber, .imageSize=image.size()};
            QJsonArray nativeSections;
            int nextOrder = 0, blockOffset = 0;
            for (int sectionIndex = 0; sectionIndex < sections.size(); ++sectionIndex) {
                const auto &section = sections[sectionIndex];
                auto part = recognizeSection(image.copy(section), croppedOcrOptions(options,section), stop, depth + 1);
                if (!part) return std::unexpected(part.error()); // Never publish only half a page.
                nativeSections.append(core::recognitionResultJson(*part));
                const auto relocate = [&](auto &regions) {
                    for (auto &region : regions) {
                        const auto prefix = QStringLiteral("tile-%1/").arg(sectionIndex);
                        if (!region.id.isEmpty()) region.id.prepend(prefix);
                        if (!region.parentId.isEmpty()) region.parentId.prepend(prefix);
                        if (region.bounds) region.bounds->translate(section.topLeft());
                        if (region.inputArea) region.inputArea->translate(section.topLeft());
                        else if (!region.bounds) region.inputArea = section;
                        region.order = region.order < 0 ? nextOrder++ : nextOrder + region.order;
                    }
                };
                relocate(part->text); relocate(part->formulas); relocate(part->tables); relocate(part->figures);
                int maximumBlock = blockOffset;
                for (auto &region : part->text) {
                    if (region.block > 0) region.block += blockOffset;
                    maximumBlock = std::max(maximumBlock, region.block);
                }
                blockOffset = maximumBlock;
                result.text += part->text; result.formulas += part->formulas;
                result.tables += part->tables; result.figures += part->figures;
                if (!part->markdown.isEmpty()) {
                    if (!result.markdown.isEmpty()) result.markdown += '\n';
                    result.markdown += part->markdown;
                }
                nextOrder = result.text.size()+result.formulas.size()+result.tables.size()+result.figures.size();
            }
            result.structuredOutput = {{"tiles", nativeSections}};
            return result;
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
        const auto root = QJsonDocument::fromJson(response).object();
        RecognitionResult result;
        result.engineId = id(); result.modelVersion = root.value("model_version").toString();
        result.provenance = {.model=root.value("model").toString(model), .task=options.formula ? "formula" : "ocr",
            .language="auto", .pageNumber=options.pageNumber, .imageSize=image.size()};
        result.structuredOutput = root;
        if (options.formula) {
            core::FormulaRegion formula; formula.latex = text; formula.order = 0;
            result.formulas.append(formula);
        } else {
            result.markdown = text;
            core::TextRegion region; region.text = text; region.order = 0;
            region.granularity = core::TextGranularity::Page;
            result.text.append(region);
        }
        return result;
    }
    auto parsed=parseResponse(response, image.size(), options, provider,engineName);
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

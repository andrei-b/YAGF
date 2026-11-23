#include "layoutcorrection.h"

#include <algorithm>
#include <QBuffer>
#include <QImageReader>
#include <QSet>
#include <QUuid>

namespace yagf::core {
namespace {
QJsonArray box(QRect rect)
{
    return {rect.x(), rect.y(), rect.width(), rect.height()};
}
}

static QString newLayoutSplitId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QJsonArray layoutAnnotations(const QVector<ImageRegion> &regions)
{
    QJsonArray rows;
    for (const auto &region : regions) {
        QJsonObject row{{"box", box(region.bounds)}, {"content", region.content},
                        {"not_text", region.notText}};
        if (!region.parts.isEmpty()) {
            QJsonArray pieces;
            for (QRect rect : region.shape()) pieces.append(box(rect));
            row["parts"] = pieces;
        }
        rows.append(row);
    }
    return rows;
}

std::expected<void, QString> recordLayoutCorrection(OcrPage &page, const QImage &image,
    const QVector<ImageRegion> &proposed, const QVector<ImageRegion> &corrected,
    const QString &source)
{
    const auto before = layoutAnnotations(proposed);
    const auto after = layoutAnnotations(corrected);
    if (before == after) return {};
    const auto fitsImage = [&](const auto &regions) {
        return std::ranges::all_of(regions, [&](const auto &r) { return image.rect().contains(r.bounds); });
    };
    if (image.size() != QSize(page.width, page.height) || image.isNull() || !fitsImage(proposed) || !fitsImage(corrected))
        return std::unexpected(QStringLiteral("The layout annotations do not match the training image."));

    auto &training = page.layoutTraining;
    if (training.splitId.isEmpty()) beginLayoutSplit(page);
    auto &examples = training.examples;
    QByteArray png;
    QString hash;
    for (const auto &example : std::as_const(examples)) {
        if (example.imageCacheKey == image.cacheKey()) {
            png = example.png;
            hash = example.imageHash;
            break;
        }
    }
    if (png.isEmpty()) {
        // Ignore color format and file metadata when matching the same processed pixels.
        const auto rgba = image.convertToFormat(QImage::Format_RGBA8888);
        const QImage pixels(rgba.constBits(), rgba.width(), rgba.height(), rgba.bytesPerLine(), rgba.format());
        QBuffer buffer(&png);
        if (!buffer.open(QIODevice::WriteOnly) || !pixels.save(&buffer, "PNG"))
            return std::unexpected(QStringLiteral("Could not encode the layout training image."));
        hash = sha256(png);
        for (const auto &example : std::as_const(examples))
            if (example.imageHash == hash) { png = example.png; break; }
    }
    const bool ocrLayout = source == "ocr-layout";
    if (source == "block-edit" || ocrLayout) {
        bool refinedProposal = false;
        for (qsizetype i = examples.size(); i-- > 0;) {
            auto &example = examples[i];
            if (example.splitId != training.splitId || example.imageHash != hash ||
                (example.source == "ocr-layout") != ocrLayout || example.corrected != before) continue;
            // Keep both the coarse-block and AI examples aligned with later edits.
            // The first correction of an unchanged AI proposal also gets its own pair.
            if (example.source != "subdivide-block") refinedProposal = true;
            if (example.proposed == after) examples.removeAt(i);
            else {
                example.corrected = after;
                example.imageCacheKey = image.cacheKey();
            }
        }
        if (refinedProposal) return {};
    }
    const auto scope = !ocrLayout && image.rect().contains(training.scope) ? training.scope : image.rect();
    examples.append({newLayoutSplitId(), training.splitId, source, hash, std::move(png), scope, before, after, image.cacheKey()});
    return {};
}

void beginLayoutSplit(OcrPage &page, QRect scope)
{
    page.layoutTraining.splitId = newLayoutSplitId();
    page.layoutTraining.scope = scope;
}

QJsonObject layoutTrainingJson(const LayoutTraining &training)
{
    QJsonObject images;
    QJsonArray examples;
    for (const auto &e : training.examples) {
        if (!images.contains(e.imageHash)) images[e.imageHash] = QString::fromLatin1(e.png.toBase64());
        examples.append(QJsonObject{{"id",e.id},{"split_id",e.splitId},{"source",e.source},{"image_sha256",e.imageHash},
                                    {"scope",box(e.scope)},{"proposed",e.proposed},{"corrected",e.corrected}});
    }
    return {{"version",1},{"split_id",training.splitId},{"scope",box(training.scope)},{"images",images},{"examples",examples}};
}

namespace {
std::optional<QRect> readBox(const QJsonValue &value)
{
    const auto v = value.toArray();
    if (v.size() != 4) return {};
    for (const auto &n : v) if (!n.isDouble() || n.toInt(-1) < 0 || n.toInt() > 1000000) return {};
    return QRect(v[0].toInt(), v[1].toInt(), v[2].toInt(), v[3].toInt());
}
bool validAnnotations(const QJsonValue &value, QRect image)
{
    if (!value.isArray()) return false;
    for (const auto &entry : value.toArray()) {
        const auto o = entry.toObject();
        auto bounds = readBox(o["box"]);
        if (!bounds || bounds->isEmpty() || !image.contains(*bounds) || !o["content"].isString() || !o["not_text"].isBool()) return false;
        if (o.contains("parts") && !o["parts"].isArray()) return false;
        for (const auto &part : o["parts"].toArray()) {
            const auto r = readBox(part);
            if (!r || r->isEmpty() || !bounds->contains(*r)) return false;
        }
    }
    return true;
}
}

std::expected<LayoutTraining, QString> readLayoutTraining(const QJsonValue &value)
{
    const auto invalid = std::unexpected(QStringLiteral("Invalid saved layout training example."));
    const auto o = value.toObject();
    const auto scope = readBox(o["scope"]);
    if (o["version"] != 1 || !scope || !o["split_id"].isString() || !o["images"].isObject() || !o["examples"].isArray()) return invalid;
    LayoutTraining training{o["split_id"].toString(), *scope, {}};
    QHash<QString, QByteArray> inputs;
    QHash<QString, QRect> frames;
    const auto images = o["images"].toObject();
    for (auto it = images.begin(); it != images.end(); ++it) {
        auto png = QByteArray::fromBase64(it.value().toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
        if (png.isEmpty() || sha256(png) != it.key()) return invalid;
        QBuffer buffer(&png);
        buffer.open(QIODevice::ReadOnly);
        const auto size = QImageReader(&buffer,"PNG").size();
        if (size.isEmpty()) return invalid;
        frames.insert(it.key(), QRect(QPoint(), size));
        inputs.insert(it.key(), std::move(png));
    }
    QSet<QString> ids;
    for (const auto &entry : o["examples"].toArray()) {
        const auto e = entry.toObject();
        const auto hash = e["image_sha256"].toString();
        const auto frame = frames.value(hash);
        const auto area = readBox(e["scope"]);
        const auto id = e["id"].toString();
        if (id.isEmpty() || ids.contains(id) || e["split_id"].toString().isEmpty() || e["source"].toString().isEmpty() ||
            !area || area->isEmpty() || !frame.contains(*area) || !validAnnotations(e["proposed"], frame) ||
            !validAnnotations(e["corrected"], frame)) return invalid;
        ids.insert(id);
        training.examples.append({id, e["split_id"].toString(), e["source"].toString(), hash, inputs.value(hash),
                                  *area, e["proposed"].toArray(), e["corrected"].toArray()});
    }
    return training;
}
}

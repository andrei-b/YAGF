#include "recognitionresult.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QHash>
#include <QSet>
#include <cmath>
#include <limits>

namespace yagf::core {
namespace {
QJsonArray boxJson(const QRect &box) { return {box.x(), box.y(), box.width(), box.height()}; }
const QStringList granularities{"word", "line", "paragraph", "block", "page"};
QJsonObject regionJson(const RecognitionRegion &region) {
    QJsonObject value{{"id", region.id}, {"parent_id", region.parentId}, {"order", region.order}, {"confidence", region.confidence}};
    if (region.bounds) value["box"] = boxJson(*region.bounds);
    if (region.inputArea) value["input_area"] = boxJson(*region.inputArea);
    return value;
}
bool integer(const QJsonValue &value, int &number) {
    if (!value.isDouble() || !std::isfinite(value.toDouble()) || value.toDouble() != std::floor(value.toDouble()) ||
        value.toDouble() < std::numeric_limits<int>::min() || value.toDouble() > std::numeric_limits<int>::max()) return false;
    number = value.toInt(); return true;
}
bool readBox(const QJsonValue &value, QRect &box) {
    if (!value.isArray() || value.toArray().size() != 4) return false;
    const auto values = value.toArray(); int v[4];
    for (int i = 0; i < 4; ++i) if (!integer(values[i], v[i])) return false;
    if (v[2] <= 0 || v[3] <= 0 ||
        qint64(v[0])+v[2] > std::numeric_limits<int>::max() || qint64(v[1])+v[3] > std::numeric_limits<int>::max()) return false;
    box = {v[0], v[1], v[2], v[3]}; return true;
}
bool readRegion(const QJsonObject &value, RecognitionRegion &region) {
    if (!value["id"].isString() || !value["parent_id"].isString() || !integer(value["order"], region.order) ||
        !value["confidence"].isDouble()) return false;
    region.id = value["id"].toString(); region.parentId = value["parent_id"].toString();
    region.confidence = value["confidence"].toDouble();
    if (value.contains("box")) { QRect box; if (!readBox(value["box"], box)) return false; region.bounds = box; }
    if (value.contains("input_area")) { QRect box; if (!readBox(value["input_area"], box)) return false; region.inputArea = box; }
    return true;
}
}

QJsonObject recognitionResultJson(const RecognitionResult &result) {
    QJsonArray text, formulas, tables, figures;
    for (const auto &region : result.text) {
        auto value = regionJson(region); value["text"] = region.text;
        value["granularity"] = granularities.value(static_cast<int>(region.granularity)); value["type"] = region.type;
        value["block"] = region.block; value["paragraph"] = region.paragraph; value["line"] = region.line; text.append(value);
    }
    for (const auto &region : result.formulas) {
        auto value = regionJson(region); value["latex"] = region.latex; value["display"] = region.display; formulas.append(value);
    }
    for (const auto &region : result.tables) {
        auto value = regionJson(region); value["text"] = region.text; value["markdown"] = region.markdown;
        value["html"] = region.html; value["structure"] = region.structure; tables.append(value);
    }
    for (const auto &region : result.figures) {
        auto value = regionJson(region); value["caption"] = region.caption; value["type"] = region.type; figures.append(value);
    }
    const auto &p = result.provenance;
    QJsonObject provenance{{"model", p.model}, {"task", p.task}, {"language", p.language}, {"page_number", p.pageNumber},
        {"width", p.imageSize.width()}, {"height", p.imageSize.height()}};
    if (!p.inputArea.isEmpty()) provenance["input_area"] = boxJson(p.inputArea);
    if(p.sourceToInput.isDefined())provenance["source_to_input"]=p.sourceToInput.toJson();
    return {{"engine_id", result.engineId}, {"model_version", result.modelVersion}, {"text", text}, {"formulas", formulas},
        {"tables", tables}, {"figures", figures}, {"markdown", result.markdown}, {"structured_output", result.structuredOutput},
        {"provenance", provenance}};
}

std::expected<void, QString> validateRecognitionResult(const RecognitionResult &result) {
    const auto fail = [] { return std::unexpected(QStringLiteral("Invalid recognition evidence.")); };
    const auto &p = result.provenance;
    if(p.sourceToInput.isDefined() && (!p.sourceToInput.validate() || p.sourceToInput.outputSize()!=QSizeF(p.imageSize)))return fail();
    if (!p.inputArea.isNull() && (p.inputArea.isEmpty() ||
        qint64(p.inputArea.x())+p.inputArea.width() > std::numeric_limits<int>::max() ||
        qint64(p.inputArea.y())+p.inputArea.height() > std::numeric_limits<int>::max())) return fail();
    if (result.engineId.size() > 256 || result.modelVersion.size() > 256 || p.model.size() > 256 || p.task.size() > 64 ||
        p.language.size() > 64 || p.pageNumber < 1 || p.imageSize.width() < 1 || p.imageSize.height() < 1 ||
        p.imageSize.width() > 100000 || p.imageSize.height() > 100000 ||
        result.text.size()+result.formulas.size()+result.tables.size()+result.figures.size() > 10000) return fail();
    QHash<QString, QString> parents;
    QStringList references;
    const auto valid = [&](const RecognitionRegion &r) {
        if (r.id.size() > 1024 || r.parentId.size() > 1024 || r.order < -1 || r.order > 1000000 ||
            !std::isfinite(r.confidence) || r.confidence < -1 || r.confidence > 100 ||
            (r.bounds && (r.bounds->isEmpty() || !QRect(QPoint(), p.imageSize).contains(*r.bounds))) ||
            (r.inputArea && (r.inputArea->isEmpty() || !QRect(QPoint(), p.imageSize).contains(*r.inputArea))) ||
            (!r.id.isEmpty() && parents.contains(r.id))) return false;
        if (!r.id.isEmpty()) parents.insert(r.id, r.parentId);
        if (!r.parentId.isEmpty()) references.append(r.parentId);
        return true;
    };
    for (const auto &r : result.text) {
        if (!valid(r) || int(r.granularity) < 0 || int(r.granularity) >= granularities.size() || r.type.isEmpty() || r.type.size() > 128 ||
            r.block < 0 || r.paragraph < 0 || r.line < 0 || r.block > 1000000 || r.paragraph > 1000000 || r.line > 1000000) return fail();
    }
    for (const auto &r : result.formulas) if (!valid(r)) return fail();
    for (const auto &r : result.tables) if (!valid(r)) return fail();
    for (const auto &r : result.figures) if (!valid(r) || r.type.isEmpty() || r.type.size() > 128) return fail();
    for (const auto &parent : references) if (!parents.contains(parent)) return fail();
    for (auto it = parents.cbegin(); it != parents.cend(); ++it) {
        QSet<QString> visited; QString id = it.key();
        while (!id.isEmpty()) {
            if (visited.contains(id) || !parents.contains(id)) return fail();
            visited.insert(id); id = parents.value(id);
            if (visited.size() > 128) return fail();
        }
    }
    if (QJsonDocument(recognitionResultJson(result)).toJson(QJsonDocument::Compact).size() > 8*1024*1024) return fail();
    return {};
}

std::expected<RecognitionResult, QString> readRecognitionResult(const QJsonObject &json) {
    const auto fail = [] { return std::unexpected(QStringLiteral("Invalid saved recognition evidence.")); };
    if (!json["engine_id"].isString() || !json["model_version"].isString() || !json["markdown"].isString() ||
        !json["structured_output"].isObject() || !json["provenance"].isObject()) return fail();
    RecognitionResult result; result.engineId = json["engine_id"].toString(); result.modelVersion = json["model_version"].toString();
    result.markdown = json["markdown"].toString(); result.structuredOutput = json["structured_output"].toObject();
    const auto p = json["provenance"].toObject(); int width, height;
    if (!p["model"].isString() || !p["task"].isString() || !p["language"].isString() ||
        !integer(p["page_number"], result.provenance.pageNumber) || !integer(p["width"], width) || !integer(p["height"], height)) return fail();
    result.provenance.model = p["model"].toString(); result.provenance.task = p["task"].toString();
    result.provenance.language = p["language"].toString(); result.provenance.imageSize = {width, height};
    if (p.contains("input_area") && !readBox(p["input_area"], result.provenance.inputArea)) return fail();
    if(p.contains("source_to_input")) {
        if(!p["source_to_input"].isObject())return fail();auto coordinates=CoordinatePipeline::fromJson(p["source_to_input"].toObject());
        if(!coordinates)return fail();result.provenance.sourceToInput=std::move(*coordinates);
    }
    for (const auto &key : {"text", "formulas", "tables", "figures"}) {
        if (!json[key].isArray() || json[key].toArray().size() > 10000) return fail();
        for (const auto &item : json[key].toArray()) {
            if (!item.isObject()) return fail(); const auto value = item.toObject();
            if (QString(key) == "text") {
                TextRegion r; int granularity = granularities.indexOf(value["granularity"].toString());
                if (!readRegion(value, r) || !value["text"].isString() || !value["type"].isString() || granularity < 0 ||
                    !integer(value["block"], r.block) || !integer(value["paragraph"], r.paragraph) || !integer(value["line"], r.line)) return fail();
                r.text = value["text"].toString(); r.type = value["type"].toString(); r.granularity = TextGranularity(granularity); result.text.append(r);
            } else if (QString(key) == "formulas") {
                FormulaRegion r; if (!readRegion(value, r) || !value["latex"].isString() || !value["display"].isBool()) return fail();
                r.latex = value["latex"].toString(); r.display = value["display"].toBool(); result.formulas.append(r);
            } else if (QString(key) == "tables") {
                TableRegion r; if (!readRegion(value, r) || !value["text"].isString() || !value["markdown"].isString() ||
                    !value["html"].isString() || !value["structure"].isObject()) return fail();
                r.text = value["text"].toString(); r.markdown = value["markdown"].toString(); r.html = value["html"].toString();
                r.structure = value["structure"].toObject(); result.tables.append(r);
            } else {
                FigureRegion r; if (!readRegion(value, r) || !value["caption"].isString() || !value["type"].isString()) return fail();
                r.caption = value["caption"].toString(); r.type = value["type"].toString(); result.figures.append(r);
            }
        }
    }
    if (auto valid = validateRecognitionResult(result); !valid) return std::unexpected(valid.error());
    return result;
}
} // namespace yagf::core

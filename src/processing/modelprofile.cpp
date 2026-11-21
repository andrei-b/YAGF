#include "modelprofile.h"
#include <QJsonArray>
#include <QRegularExpression>
#include <cmath>

namespace yagf::processing {
QString canonicalModelTask(const QString &task) { return task == "ocr" ? QStringLiteral("text") : task; }
QJsonObject modelProfileJson(const ModelProfile &p) {
    QJsonObject json{{"model",p.model},{"tasks",QJsonArray::fromStringList(p.tasks)},
        {"geometry",p.geometry},{"formats",QJsonArray::fromStringList(p.formats)},
        {"languages",QJsonArray::fromStringList(p.languages)}};
    if(p.maxImagePixels)json["max_image_pixels"]=double(p.maxImagePixels);
    if(p.supportsBatching)json["supports_batching"]=*p.supportsBatching;
    return json;
}
std::expected<ModelProfile,QString> readModelProfile(const QJsonValue &json) {
    const auto o=json.toObject(); ModelProfile p;
    if(!json.isObject() || !o["model"].isString() || o["model"].toString().trimmed().isEmpty() || o["model"].toString().size()>256)
        return std::unexpected(QStringLiteral("Invalid model profile name."));
    p.model=o["model"].toString();
    const auto list=[&](const QString &key,QStringList &out,int limit,bool required=false) {
        if(!o.contains(key))return !required;
        if(!o[key].isArray() || o[key].toArray().size()>limit || (required && o[key].toArray().isEmpty()))return false;
        static const QRegularExpression token("^[A-Za-z0-9_+.*-]{1,64}$");
        for(const auto &v:o[key].toArray()) {
            if(!v.isString() || !token.match(v.toString()).hasMatch())return false;
            const auto value=key=="tasks"?canonicalModelTask(v.toString()):v.toString();
            if(!out.contains(value))out.append(value);
        }
        return true;
    };
    if(!list("tasks",p.tasks,32,true) || !list("formats",p.formats,32) || !list("languages",p.languages,128))
        return std::unexpected(QStringLiteral("Invalid model profile capabilities."));
    if(o.contains("geometry")) {
        if(!o["geometry"].isString() || !QStringList{"unknown","none","page","block","line","word"}.contains(o["geometry"].toString()))
            return std::unexpected(QStringLiteral("Invalid model profile geometry."));
        p.geometry=o["geometry"].toString();
    }
    if(o.contains("max_image_pixels")) {
        const double pixels=o["max_image_pixels"].toDouble(-1);
        if(!o["max_image_pixels"].isDouble() || !std::isfinite(pixels) || pixels<1 || pixels>9007199254740991. || std::floor(pixels)!=pixels)
            return std::unexpected(QStringLiteral("Invalid model image limit."));
        p.maxImagePixels=qint64(pixels);
    }
    if(o.contains("supports_batching")) {
        if(!o["supports_batching"].isBool())return std::unexpected(QStringLiteral("Invalid model batching capability."));
        p.supportsBatching=o["supports_batching"].toBool();
    }
    return p;
}
QStringList ModelProfile::incompatibilities(const ModelRequirements &r) const {
    QStringList errors;
    for(const auto &task:r.tasks)if(!tasks.contains(canonicalModelTask(task)))errors.append(QStringLiteral("Task %1 is not advertised.").arg(task));
    // Geometry denotes the actual granularity of evidence, not fabricated boxes.
    if(!r.geometry.isEmpty() && geometry!=r.geometry)errors.append(QStringLiteral("Required %1 geometry is not advertised.").arg(r.geometry));
    if(!r.format.isEmpty() && !formats.contains(r.format))errors.append(QStringLiteral("Output format %1 is not advertised.").arg(r.format));
    if(!r.language.isEmpty() && !languages.contains(r.language))errors.append(QStringLiteral("Language %1 is not advertised.").arg(r.language));
    if(r.imageSize.isValid()) {
        if(!maxImagePixels)errors.append(QStringLiteral("Model image limit is not advertised."));
        else if(qint64(r.imageSize.width())*r.imageSize.height()>maxImagePixels)
            errors.append(QStringLiteral("Image exceeds the model limit of %1 pixels.").arg(maxImagePixels));
    }
    if(r.batching && supportsBatching!=true)errors.append(QStringLiteral("Batch requests are not advertised."));
    return errors;
}
}

#include "coordinateframes.h"
#include <QJsonDocument>

namespace yagf::core {
QJsonObject CoordinateFrames::encode(const RecognitionResult &result) {
    const auto &mapping=result.provenance.sourceToInput;
    if(!mapping.isDefined())return recognitionResultJson(result);
    auto base=mapping;QVector<CoordinateStep> tail;
    while(!base.steps.isEmpty() && base.steps.last().operation=="recognition-crop")tail.prepend(base.steps.takeLast());
    QString key;
    for(auto it=frames.cbegin();it!=frames.cend();++it)if(it.value()==base){key=it.key();break;}
    if(key.isEmpty()){key=QStringLiteral("frame-%1").arg(frames.size()+1);frames.insert(key,base);}
    auto copy=result;copy.provenance.sourceToInput={};auto json=recognitionResultJson(copy);
    CoordinatePipeline suffix;suffix.sourceSize=base.outputSize();suffix.steps=std::move(tail);
    auto provenance=json["provenance"].toObject();provenance["source_to_input"]=QJsonObject{{"frame",key},{"tail",suffix.toJson()}};
    json["provenance"]=provenance;return json;
}
std::expected<RecognitionResult,QString> CoordinateFrames::decode(const QJsonObject &json) const {
    const auto reference=json["provenance"].toObject()["source_to_input"].toObject();
    if(!reference.contains("frame"))return readRecognitionResult(json);
    const auto key=reference["frame"].toString();
    if(!frames.contains(key) || !reference["tail"].isObject())return std::unexpected(QStringLiteral("Unknown recognition coordinate frame."));
    auto suffix=CoordinatePipeline::fromJson(reference["tail"].toObject());
    if(!suffix || suffix->sourceSize!=frames[key].outputSize())return std::unexpected(QStringLiteral("Invalid recognition crop transform."));
    auto value=json;auto provenance=json["provenance"].toObject();provenance.remove("source_to_input");value["provenance"]=provenance;
    auto result=readRecognitionResult(value);if(!result)return result;
    auto mapping=frames[key];mapping.steps+=suffix->steps;
    // Both pieces were validated independently; their shared boundary matches.
    // Avoid revalidating a large shared mesh for every crop on the page.
    if(mapping.steps.size()>32 || mapping.outputSize()!=QSizeF(result->provenance.imageSize))return std::unexpected(QStringLiteral("Invalid recognition coordinate frame."));
    result->provenance.sourceToInput=std::move(mapping);return result;
}
QJsonObject CoordinateFrames::toJson() const {QJsonObject out;for(auto it=frames.cbegin();it!=frames.cend();++it)out[it.key()]=it->toJson();return out;}
std::expected<void,QString> CoordinateFrames::load(const QJsonObject &json) {
    if(json.size()>64 || QJsonDocument(json).toJson(QJsonDocument::Compact).size()>16*1024*1024)return std::unexpected(QStringLiteral("Too many recognition coordinate frames."));
    QHash<QString,CoordinatePipeline> parsed;
    for(auto it=json.begin();it!=json.end();++it) {
        if(it.key().isEmpty() || it.key().size()>128 || !it->isObject())return std::unexpected(QStringLiteral("Invalid recognition coordinate frame."));
        auto frame=CoordinatePipeline::fromJson(it->toObject());if(!frame)return std::unexpected(frame.error());parsed.insert(it.key(),std::move(*frame));
    }frames=std::move(parsed);return {};
}
}

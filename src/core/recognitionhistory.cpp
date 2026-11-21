#include "recognitionhistory.h"
#include "coordinateframes.h"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QImageReader>
#include <QBuffer>
#include <QSet>
#include <QDateTime>
#include <cmath>

namespace yagf::core {
namespace {
QJsonArray box(QRect r){return {r.x(),r.y(),r.width(),r.height()};}
bool integer(const QJsonValue &v,qint64 low,qint64 high){const double n=v.toDouble(-1);return v.isDouble() && std::isfinite(n) && std::floor(n)==n && n>=low && n<=high;}
bool hash(const QString &value){if(value.isEmpty())return true;if(value.size()!=64)return false;for(auto c:value)if(!QStringLiteral("0123456789abcdef").contains(c))return false;return true;}
QJsonObject encodeFrame(const CoordinatePipeline &pipeline,QSize size,CoordinateFrames *frames){
    if(!pipeline.isDefined())return {};
    if(!frames)return pipeline.toJson();
    RecognitionResult stub;stub.provenance.imageSize=size;stub.provenance.sourceToInput=pipeline;
    return frames->encode(stub)["provenance"].toObject()["source_to_input"].toObject();
}
std::expected<CoordinatePipeline,QString> decodeFrame(const QJsonValue &value,QSize size,const CoordinateFrames *frames){
    if(!value.isObject())return std::unexpected(QStringLiteral("Invalid history coordinate frame."));
    if(!frames || !value.toObject().contains("frame"))return CoordinatePipeline::fromJson(value.toObject());
    RecognitionResult stub;stub.provenance.imageSize=size;auto json=recognitionResultJson(stub);
    auto p=json["provenance"].toObject();p["source_to_input"]=value;json["provenance"]=p;
    auto result=frames->decode(json);if(!result)return std::unexpected(result.error());return result->provenance.sourceToInput;
}
}
QString sha256(const QByteArray &bytes){return QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());}
QJsonObject preprocessingEvidence(const PreprocessSettings &s,int dpi){
    const auto &m=s.perspective;
    QJsonObject out{{"version",1},{"render_dpi",dpi},{"deskew_degrees",s.deskewDegrees},{"auto_contrast",s.autoContrast},
        {"denoise",s.denoise},{"quarter_turns",s.quarterTurns},{"adaptive_binarization",s.adaptiveBinarization},
        {"ink_threshold",s.inkThreshold},{"bend_percent",s.bendPercent},{"level_shadows",s.levelShadows},
        {"perspective",QJsonArray{m.m11(),m.m12(),m.m13(),m.m21(),m.m22(),m.m23(),m.m31(),m.m32(),m.m33()}}};
    if(!s.restoredPng.isEmpty())out["restored_image_sha256"]=sha256(s.restoredPng);
    if(s.restorationGeometry.isDefined())out["restoration_geometry"]=s.restorationGeometry.toJson();
    return out;
}
QJsonObject recognitionHistoryJson(const RecognitionHistory &history,CoordinateFrames *frames){
    QJsonArray runs,decisions;QJsonObject inputs;
    for(const auto &r:history.runs){
        QJsonArray source;for(const auto &point:r.sourceCrop)source.append(QJsonArray{point.x(),point.y()});
        QJsonObject run{{"id",r.id},{"session",r.session},{"role",r.role},{"source",r.source},{"engine",r.engine},
            {"model",r.model},{"model_version",r.version},{"model_hash",r.modelHash},{"source_sha256",r.sourceHash},
            {"input_sha256",r.inputHash},{"preprocessing_sha256",r.preprocessingHash},{"started_utc",r.startedUtc},
            {"status",r.status},{"error",r.error},{"elapsed_ms",double(r.elapsedMs)},
            {"input_size",QJsonArray{r.inputSize.width(),r.inputSize.height()}},{"processed_crop",box(r.processedCrop)},
            {"source_crop",source},{"preprocessing",r.preprocessing},{"output",r.output},{"runtime",r.runtime},
            {"warnings",QJsonArray::fromStringList(r.warnings)}};
        if(r.sourceToInput.isDefined())run["source_to_input"]=encodeFrame(r.sourceToInput,r.inputSize,frames);
        if(!r.inputPng.isEmpty())inputs[r.inputHash]=QString::fromLatin1(r.inputPng.toBase64());
        runs.append(run);
    }
    for(const auto &d:history.decisions){
        QJsonObject value{{"id",d.id},{"run_id",d.runId},{"decision",d.decision},{"timestamp_utc",d.timestampUtc},
            {"proposed_text",d.proposedText},{"primary_text",d.primaryText},{"word_ids",QJsonArray::fromStringList(d.wordIds)},
            {"processed_area",box(d.processedArea)}};
        if(d.sourceToProcessed.isDefined())value["source_to_processed"]=encodeFrame(d.sourceToProcessed,d.sourceToProcessed.outputSize().toSize(),frames);
        decisions.append(value);
    }
    return {{"version",1},{"runs",runs},{"decisions",decisions},{"inputs",inputs}};
}
std::expected<void,QString> validateRecognitionHistory(const RecognitionHistory &history){
    const auto fail=[] {return std::unexpected(QStringLiteral("Invalid or oversized recognition history; no history was discarded."));};
    if(history.runs.size()>10000 || history.decisions.size()>10000)return fail();
    QSet<QString> ids;qsizetype bytes=0;QHash<QString,QSize> images;QHash<QString,QByteArray> imageData;
    for(const auto &r:history.runs){
        if(r.id.isEmpty() || r.id.size()>128 || ids.contains(r.id) || r.session.size()>128 || r.role.size()>64 || r.source.size()>256 ||
            r.engine.isEmpty() || r.engine.size()>256 || r.model.size()>256 || r.version.size()>256 || r.modelHash.size()>256 ||
            !hash(r.sourceHash) || !hash(r.inputHash) || !hash(r.preprocessingHash) ||
            !QDateTime::fromString(r.startedUtc,Qt::ISODateWithMs).isValid() || r.elapsedMs<0 || r.elapsedMs>604800000 ||
            !QStringList{"succeeded","failed","cancelled"}.contains(r.status) || r.error.size()>65536 || r.warnings.size()>1000 ||
            r.inputSize.width()<0 || r.inputSize.height()<0 ||
            ((r.inputSize.isEmpty()) && (r.status=="succeeded" || r.inputSize!=QSize(0,0))) || r.inputSize.width()>20000 || r.inputSize.height()>20000 ||
            qint64(r.inputSize.width())*r.inputSize.height()>80000000 || r.processedCrop.size()!=r.inputSize ||
            std::abs(qint64(r.processedCrop.x()))>1000000 || std::abs(qint64(r.processedCrop.y()))>1000000 || r.sourceCrop.size()>1024)return fail();
        ids.insert(r.id);
        for(const auto &point:r.sourceCrop)if(!std::isfinite(point.x()) || !std::isfinite(point.y()) || std::abs(point.x())>1000000 || std::abs(point.y())>1000000)return fail();
        for(const auto &warning:r.warnings)if(warning.size()>65536)return fail();
        if(!r.preprocessingHash.isEmpty() && sha256(QJsonDocument(r.preprocessing).toJson(QJsonDocument::Compact))!=r.preprocessingHash)return fail();
        if(r.sourceToInput.isDefined() && (!r.sourceToInput.validate() || r.sourceToInput.outputSize()!=QSizeF(r.inputSize)))return fail();
        if(r.inputPng.isEmpty()!=r.inputHash.isEmpty())return fail();
        if(images.contains(r.inputHash) && (images[r.inputHash]!=r.inputSize || imageData[r.inputHash]!=r.inputPng))return fail();
        if(!r.inputPng.isEmpty() && !images.contains(r.inputHash)){
            if(r.inputPng.size()>32*1024*1024 || sha256(r.inputPng)!=r.inputHash)return fail();
            QBuffer buffer;buffer.setData(r.inputPng);buffer.open(QIODevice::ReadOnly);QImageReader reader(&buffer,"PNG");
            if(reader.size()!=r.inputSize)return fail();images.insert(r.inputHash,r.inputSize);imageData.insert(r.inputHash,r.inputPng);bytes+=r.inputPng.size();
        }
        bytes+=r.error.size()*2;for(const auto &warning:r.warnings)bytes+=warning.size()*2;
        bytes+=QJsonDocument(r.runtime).toJson(QJsonDocument::Compact).size();
        bytes+=QJsonDocument(r.output).toJson(QJsonDocument::Compact).size()+QJsonDocument(r.preprocessing).toJson(QJsonDocument::Compact).size();
        if(bytes>256*1024*1024)return fail();
    }
    QSet<QString> decisions;
    for(const auto &d:history.decisions){
        if(d.id.isEmpty() || d.id.size()>128 || decisions.contains(d.id) || !ids.contains(d.runId) ||
            !QStringList{"accepted","rejected"}.contains(d.decision) || !QDateTime::fromString(d.timestampUtc,Qt::ISODateWithMs).isValid() ||
            d.proposedText.size()>65536 || d.primaryText.size()>65536 || d.wordIds.size()>10000 || d.processedArea.isEmpty() ||
            (d.sourceToProcessed.isDefined() && !d.sourceToProcessed.validate()))return fail();
        decisions.insert(d.id);for(const auto &id:d.wordIds){if(id.size()>1024)return fail();bytes+=id.size()*2;}
        bytes+=(d.proposedText.size()+d.primaryText.size())*2;
        if(bytes>256*1024*1024)return fail();
    }
    return {};
}
std::expected<RecognitionHistory,QString> readRecognitionHistory(const QJsonValue &json,const CoordinateFrames *frames){
    const auto fail=[] {return std::unexpected(QStringLiteral("Invalid saved recognition history."));};
    const auto root=json.toObject();if(!json.isObject() || root["version"]!=1 || !root["runs"].isArray() || !root["decisions"].isArray() || !root["inputs"].isObject() || root["runs"].toArray().size()>10000 || root["decisions"].toArray().size()>10000 || root["inputs"].toObject().size()>10000)return fail();
    QHash<QString,QByteArray> inputs;qsizetype bytes=0;
    const auto assets=root["inputs"].toObject();
    for(auto it=assets.begin();it!=assets.end();++it){
        if(!it->isString() || it->toString().size()>44*1024*1024)return fail();
        auto data=QByteArray::fromBase64Encoding(it->toString().toLatin1(),QByteArray::AbortOnBase64DecodingErrors);
        if(!data || sha256(data.decoded)!=it.key())return fail();bytes+=data.decoded.size();if(bytes>256*1024*1024)return fail();inputs.insert(it.key(),std::move(data.decoded));
    }
    const auto strings=[&](const QJsonValue &value,QStringList &out,int max){if(!value.isArray() || value.toArray().size()>max)return false;for(const auto &v:value.toArray()){if(!v.isString())return false;out.append(v.toString());}return true;};
    const auto readRect=[&](const QJsonValue &value,QRect &rect,bool allowEmpty=false){const auto a=value.toArray();if(!value.isArray() || a.size()!=4)return false;for(const auto &v:a)if(!integer(v,-1000000,1000000))return false;rect={a[0].toInt(),a[1].toInt(),a[2].toInt(),a[3].toInt()};return allowEmpty || !rect.isEmpty();};
    RecognitionHistory result;
    for(const auto &value:root["runs"].toArray()){
        if(!value.isObject())return fail();const auto o=value.toObject();RecognitionRun r;
        for(const auto *key:{"id","session","role","source","engine","model","model_version","model_hash","source_sha256","input_sha256","preprocessing_sha256","started_utc","status","error"})if(!o[key].isString())return fail();
        r.id=o["id"].toString();r.session=o["session"].toString();r.role=o["role"].toString();r.source=o["source"].toString();r.engine=o["engine"].toString();r.model=o["model"].toString();r.version=o["model_version"].toString();r.modelHash=o["model_hash"].toString();r.sourceHash=o["source_sha256"].toString();r.inputHash=o["input_sha256"].toString();r.preprocessingHash=o["preprocessing_sha256"].toString();r.startedUtc=o["started_utc"].toString();r.status=o["status"].toString();r.error=o["error"].toString();
        const auto size=o["input_size"].toArray();if(size.size()!=2 || !integer(size[0],0,20000) || !integer(size[1],0,20000) || !integer(o["elapsed_ms"],0,604800000) || !readRect(o["processed_crop"],r.processedCrop,true) || !o["preprocessing"].isObject() || !o["output"].isObject() || !o["runtime"].isObject() || !o["source_crop"].isArray() || o["source_crop"].toArray().size()>1024 || !strings(o["warnings"],r.warnings,1000))return fail();
        r.inputSize={size[0].toInt(),size[1].toInt()};r.elapsedMs=qint64(o["elapsed_ms"].toDouble());r.preprocessing=o["preprocessing"].toObject();r.output=o["output"].toObject();r.runtime=o["runtime"].toObject();
        if(!r.inputHash.isEmpty()){if(!inputs.contains(r.inputHash))return fail();r.inputPng=inputs[r.inputHash];}
        for(const auto &v:o["source_crop"].toArray()){const auto a=v.toArray();if(a.size()!=2 || !a[0].isDouble() || !a[1].isDouble())return fail();r.sourceCrop.append({a[0].toDouble(),a[1].toDouble()});}
        if(o.contains("source_to_input")){auto frame=decodeFrame(o["source_to_input"],r.inputSize,frames);if(!frame)return fail();r.sourceToInput=std::move(*frame);}
        result.runs.append(std::move(r));
    }
    for(const auto &value:root["decisions"].toArray()){
        if(!value.isObject())return fail();const auto o=value.toObject();RecognitionDecision d;
        for(const auto *key:{"id","run_id","decision","timestamp_utc","proposed_text","primary_text"})if(!o[key].isString())return fail();
        d.id=o["id"].toString();d.runId=o["run_id"].toString();d.decision=o["decision"].toString();d.timestampUtc=o["timestamp_utc"].toString();d.proposedText=o["proposed_text"].toString();d.primaryText=o["primary_text"].toString();
        if(!readRect(o["processed_area"],d.processedArea) || !strings(o["word_ids"],d.wordIds,10000))return fail();
        if(o.contains("source_to_processed")){
            const auto frameValue=o["source_to_processed"].toObject();
            QSize size;
            // Frame references carry a validated tail with its output size.
            if(frameValue.contains("tail")){auto suffix=CoordinatePipeline::fromJson(frameValue["tail"].toObject());if(!suffix)return fail();size=suffix->outputSize().toSize();}
            else {auto direct=CoordinatePipeline::fromJson(frameValue);if(!direct)return fail();size=direct->outputSize().toSize();}
            auto frame=decodeFrame(o["source_to_processed"],size,frames);if(!frame)return fail();d.sourceToProcessed=std::move(*frame);
        }
        result.decisions.append(std::move(d));
    }
    if(auto valid=validateRecognitionHistory(result);!valid)return std::unexpected(valid.error());return result;
}
void mergeRecognitionHistory(RecognitionHistory &target,const RecognitionHistory &incoming){
    QSet<QString> runs,decisions;for(const auto &r:target.runs)runs.insert(r.id);for(const auto &d:target.decisions)decisions.insert(d.id);
    for(const auto &r:incoming.runs)if(!runs.contains(r.id)){target.runs.append(r);runs.insert(r.id);}
    for(const auto &d:incoming.decisions)if(!decisions.contains(d.id)){target.decisions.append(d);decisions.insert(d.id);}
}
}

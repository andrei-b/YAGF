#include "recognitionrecorder.h"
#include <QBuffer>
#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <exception>
#ifdef Q_OS_LINUX
#include <sys/stat.h>
#endif
#include <QUuid>

namespace yagf::processing {
RecognitionRecorder::RecognitionRecorder(QString hash,QJsonObject settings):
    session(QUuid::createUuid().toString(QUuid::WithoutBraces)),sourceHash(std::move(hash)),preprocessing(std::move(settings)){}
core::RecognitionRun RecognitionRecorder::begin(const QString &engine,const QString &model,const QImage &image,const OcrOptions &options){
    core::RecognitionRun run;run.id=QUuid::createUuid().toString(QUuid::WithoutBraces);run.session=session;
    run.engine=engine;run.model=model;run.role=options.historyRole;run.source=options.historySource;
    run.startedUtc=QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    run.sourceHash=sourceHash;run.preprocessing=preprocessing;
    run.preprocessing["recognition_options"]=QJsonObject{{"language",options.language},{"page_number",options.pageNumber},
        {"task",options.task},{"formula",options.formula},{"output_format",options.outputFormat},{"single_text_line",options.singleTextLine},
        {"preserve_formatting",options.preserveFormatting},{"user_words",QJsonArray::fromStringList(options.userWords)},{"user_words_sha256",core::sha256(options.userWords.join('\n').toUtf8())}};
    run.preprocessingHash=core::sha256(QJsonDocument(run.preprocessing).toJson(QJsonDocument::Compact));
    run.inputSize=image.size();run.processedCrop=QRect(options.historyOffset,image.size());run.sourceToInput=options.historyCoordinates;
    if(run.sourceToInput.isDefined() && run.sourceToInput.outputSize()!=QSizeF(image.size())) {
        run.sourceToInput={};run.warnings.append(QStringLiteral("The supplied coordinate frame does not match this input; the source mapping is unknown."));
    }
    if(run.sourceToInput.isDefined()){
        auto outline=run.sourceToInput.mapOutline(core::rectangleOutline(image.rect()),true);
        if(outline)run.sourceCrop=*outline;else run.warnings.append(QStringLiteral("Source crop mapping unavailable: ")+outline.error());
    }
    if(!image.isNull()){
        QBuffer buffer(&run.inputPng);buffer.open(QIODevice::WriteOnly);
        if(image.save(&buffer,"PNG")){
            run.inputHash=core::sha256(run.inputPng);
            std::lock_guard lock(mutex);auto found=images.constFind(run.inputHash);
            if(found!=images.cend())run.inputPng=*found;else images.insert(run.inputHash,run.inputPng);
        }else {run.inputPng.clear();run.warnings.append(QStringLiteral("The recognition crop could not be encoded."));}
    }
    return run;
}
core::RecognitionRun RecognitionRecorder::finish(core::RecognitionRun run,qint64 elapsed,const QString &status,const QString &error,
    QJsonObject output,QJsonObject runtime,QStringList warnings){
    run.elapsedMs=elapsed;run.status=status;run.error=error;run.output=std::move(output);run.warnings+=warnings;
    const auto provenance=run.output["provenance"].toObject();
    if(provenance["model"].isString() && !provenance["model"].toString().isEmpty())run.model=provenance["model"].toString().left(256);
    auto raw=run.output["structured_output"].toObject();
    auto responses=run.output["responses"].toArray();
    if(responses.isEmpty())responses=run.output["engine_output"].toObject()["responses"].toArray();
    if(raw.isEmpty() && !responses.isEmpty())raw=responses.last().toObject()["response"].toObject();
    if(raw["model"].isString() && !raw["model"].toString().isEmpty())run.model=raw["model"].toString().left(256);
    run.version=run.output["model_version"].toString().left(256);
    if(run.version.isEmpty())run.version=raw["model_version"].toString().left(256);
    auto details=run.output["structured_output"].toObject()["run_details"].toObject();
    if(details.isEmpty())details=run.output["run_details"].toObject();
    if(details.isEmpty())details=raw["run_details"].toObject();
    for(auto it=runtime.begin();it!=runtime.end();++it)details[it.key()]=*it;
    for(const auto *name:{"model_hash","model_sha256","sha256"}){
        auto value=details.value(name).toString();if(value.isEmpty())value=run.output.value(name).toString();if(value.isEmpty())value=raw.value(name).toString();
        if(!value.isEmpty()){run.modelHash=value.left(256);break;}
    }
    if(run.version.isEmpty())run.version=details["model_version"].toString().left(256);
    if(run.version.isEmpty())run.version=details["version"].toString().left(256);
    for(const auto *name:{"inference_ms","queue_ms","peak_rss_bytes"})if(details.value(name).isDouble())run.runtime[name]=details.value(name);
    for(const auto &v:run.output["warnings"].toArray())if(v.isString())run.warnings.append(v.toString());
    for(const auto &v:raw["warnings"].toArray())if(v.isString())run.warnings.append(v.toString());
    std::lock_guard lock(mutex);history.runs.append(run);return run;
}
core::RecognitionHistory RecognitionRecorder::snapshot() const {std::lock_guard lock(mutex);return history;}
OcrOptions croppedOcrOptions(const OcrOptions &options,QRect crop,int padding){
    auto copy=options;copy.historyOffset+=crop.topLeft()-QPoint(padding,padding);
    if(copy.historyCoordinates.isDefined())copy.historyCoordinates.appendProjective("recognition-crop",
        QTransform::fromTranslate(-crop.x()+padding,-crop.y()+padding),crop.size()+QSize(2*padding,2*padding));
    return copy;
}
std::expected<core::OcrPage,OcrError> IOcrEngine::recognize(const QImage &image,const OcrOptions &options,std::stop_token stop){
    core::RecognitionRun run;if(options.history)run=options.history->begin(id(),modelIdentifier(),image,options);
    QElapsedTimer timer;timer.start();auto settings=options;settings.historyNativeCall=true;
    std::expected<RecognitionResult,OcrError> native=std::unexpected(OcrError{});
    try { native=recognizeResult(image,settings,stop); }
    catch(const std::exception &e) {native=std::unexpected(OcrError{QString::fromUtf8(e.what())});}
    catch(...) {native=std::unexpected(OcrError{QStringLiteral("Recognition raised an unknown exception.")});}
    QJsonObject output=native?core::recognitionResultJson(*native):lastStructuredOutput();
    const auto raw=lastStructuredOutput();if(native && !raw.isEmpty() && raw!=native->structuredOutput) output["engine_output"]=raw;
    std::expected<core::OcrPage,OcrError> page=std::unexpected(OcrError{});
    if(!native)page=std::unexpected(native.error());
    else if(stop.stop_requested())page=std::unexpected(OcrError{{},true});
    else {
        if(native->engineId.isEmpty())native->engineId=id();
        auto adapted=recognitionToPage(*native);page=adapted?std::expected<core::OcrPage,OcrError>(std::move(*adapted)):std::unexpected(OcrError{adapted.error()});
    }
    if(options.history) {auto evidence=options.history->finish(std::move(run),timer.elapsed(),page?"succeeded":page.error().cancelled?"cancelled":"failed",
        page?QString():page.error().message,std::move(output),lastRunDetails());
        if(page)page->recognitionHistory.runs.append(std::move(evidence));
    }
    return page;
}
QString recognitionSourceHash(const QString &path,std::stop_token stop){
    // Cache by file identity only in memory. Paths and host metadata are never
    // serialized into recognition evidence.
    static std::mutex mutex;static QHash<QString,QString> cache;
    QFileInfo info(path);QString key=info.absoluteFilePath()+':'+QString::number(info.size())+':'+QString::number(info.lastModified().toMSecsSinceEpoch());
#ifdef Q_OS_LINUX
    struct stat identity{};
    if(::stat(QFile::encodeName(path).constData(),&identity)==0)
        key+=':'+QString::number(identity.st_ino)+':'+QString::number(identity.st_mtim.tv_nsec)+':'+QString::number(identity.st_ctim.tv_sec)+':'+QString::number(identity.st_ctim.tv_nsec);
#endif
    {std::lock_guard lock(mutex);if(cache.contains(key))return cache[key];}
    QFile file(path);if(!file.open(QIODevice::ReadOnly))return {};
    QCryptographicHash digest(QCryptographicHash::Sha256);
    while(!file.atEnd()){if(stop.stop_requested())return {};const auto bytes=file.read(1024*1024);if(bytes.isEmpty() && file.error()!=QFileDevice::NoError)return {};digest.addData(bytes);}
    const auto hash=QString::fromLatin1(digest.result().toHex());
    {std::lock_guard lock(mutex);if(cache.size()>64)cache.clear();cache[key]=hash;}return hash;
}
}

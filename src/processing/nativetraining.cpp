#include "nativetraining.h"
#include <QBuffer>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonDocument>
#include <QImageReader>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSet>
#include <QTimer>
#include <cmath>

namespace yagf::processing {
QJsonObject trainingSampleJson(const NativeTrainingSample &s)
{
    return {{"id",s.id},{"page",s.page},{"box",QJsonArray{s.bounds.x(),s.bounds.y(),s.bounds.width(),s.bounds.height()}},
        {"text",s.text},{"png_base64",QString::fromLatin1(s.png.toBase64())},{"decision",s.decision},
        {"partition",s.partition},{"previously_reviewed",s.previouslyReviewed}};
}
std::expected<QVector<NativeTrainingSample>,QString> readTrainingSamples(const QJsonValue &value)
{
    const auto fail=[] {return std::unexpected(QStringLiteral("Invalid native OCR training examples."));};
    if (!value.isArray() || value.toArray().size()>5000) return fail();
    QVector<NativeTrainingSample> samples; QSet<QString> ids; qint64 total=0;
    for (const auto &entry:value.toArray()) {
        const auto o=entry.toObject(); NativeTrainingSample s;
        s.id=o["id"].toString(); s.page=o["page"].toInt(-1); s.text=o["text"].toString();
        s.decision=o["decision"].toString(); s.partition=o["partition"].toString(); s.previouslyReviewed=o["previously_reviewed"].toBool();
        const auto box=o["box"].toArray();
        if (!entry.isObject() || s.id.isEmpty() || s.id.size()>128 || ids.contains(s.id) || s.page<1 || s.page>100000 ||
            (s.text.isEmpty() && s.decision=="approved") || s.text.size()>4096 || s.text.trimmed()!=s.text ||
            (s.decision!="approved" && s.decision!="pending" && s.decision!="excluded") ||
            (s.partition!="train" && s.partition!="validation") || box.size()!=4) return fail();
        for (char32_t c:s.text.toUcs4()) if (!QChar::isPrint(c) || (QChar::isSpace(c) && c!=' ')) return fail();
        for (const auto &v:box) if (!v.isDouble() || !std::isfinite(v.toDouble()) || std::floor(v.toDouble())!=v.toDouble() || v.toDouble()<0 || v.toDouble()>20000) return fail();
        s.bounds={box[0].toInt(),box[1].toInt(),box[2].toInt(),box[3].toInt()};
        const auto encoded=o["png_base64"].toString().toLatin1();
        const auto decoded=QByteArray::fromBase64Encoding(encoded,QByteArray::AbortOnBase64DecodingErrors);
        if (!decoded) return fail(); s.png=decoded.decoded; total+=s.png.size();
        if (s.png.size()>2*1024*1024 || total>32*1024*1024 || s.bounds.isEmpty()) return fail();
        QBuffer buffer(&s.png); buffer.open(QIODevice::ReadOnly); QImageReader reader(&buffer,"PNG");
        const auto size=reader.size();
        if (!size.isValid() || size!=s.bounds.size() || size.width()>16000 || size.height()>4000 || qint64(size.width())*size.height()>8000000 || reader.read().isNull()) return fail();
        ids.insert(s.id); samples.append(std::move(s));
    }
    return samples;
}
std::expected<QJsonObject,OcrError> nativeTrainingRequest(const QUrl &base,const QString &token,const QByteArray &method,
    const QString &path,const QJsonObject &body,std::stop_token stop)
{
    const auto fail=[](QString message){return std::unexpected(OcrError{std::move(message)});};
    if (!base.isValid() || (base.scheme()!="http" && base.scheme()!="https") || base.host().isEmpty() || !base.userInfo().isEmpty() ||
        base.hasQuery() || base.hasFragment() || (!base.path().isEmpty() && base.path()!="/")) return fail(QStringLiteral("Enter the native training host's HTTP(S) base URL."));
    if (stop.stop_requested()) return std::unexpected(OcrError{{},true});
    QUrl url=base; url.setPath(path);
    QNetworkAccessManager manager; QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,QNetworkRequest::ManualRedirectPolicy);
    request.setHeader(QNetworkRequest::ContentTypeHeader,"application/json");
    if (!token.isEmpty()) request.setRawHeader("Authorization",("Bearer "+token).toUtf8());
    const auto payload=QJsonDocument(body).toJson(QJsonDocument::Compact);
    if (payload.size()>48*1024*1024) return fail(QStringLiteral("Training dataset is too large; select fewer examples."));
    auto *reply=method=="GET" ? manager.get(request) : manager.post(request,payload);
    QEventLoop loop; QTimer timer; QElapsedTimer elapsed; elapsed.start(); QByteArray bytes; bool oversized=false,timedOut=false;
    const auto read=[&]{if (!reply->isOpen()) return; bytes+=reply->readAll(); if (bytes.size()>8*1024*1024) {oversized=true; reply->abort();}};
    QObject::connect(reply,&QNetworkReply::readyRead,&loop,read); QObject::connect(reply,&QNetworkReply::finished,&loop,&QEventLoop::quit);
    QObject::connect(&timer,&QTimer::timeout,&loop,[&]{if (stop.stop_requested() || elapsed.elapsed()>120000) {timedOut=!stop.stop_requested(); reply->abort();}});
    timer.start(40); loop.exec(); read();
    if (stop.stop_requested()) return std::unexpected(OcrError{{},true});
    if (oversized || timedOut) return fail(oversized ? QStringLiteral("Training host response is too large.") : QStringLiteral("Training host timed out."));
    const auto json=QJsonDocument::fromJson(bytes); const auto response=json.object();
    if (reply->error()!=QNetworkReply::NoError) return fail(response["error"].toString(reply->errorString()));
    if (!json.isObject() || response["protocol"]!="yagf-native-training/1") return fail(QStringLiteral("Incompatible native training host."));
    return response;
}
}

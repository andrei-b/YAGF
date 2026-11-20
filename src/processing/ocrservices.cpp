#include "ocrservices.h"
#include "remoteocrengine.h"
#include "tesseractengine.h"
#include "mathocrengine.h"
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QTimer>
#include <algorithm>
#include <vector>

namespace yagf::processing {
namespace {
QUrl normalized(QUrl url) {url=url.adjusted(QUrl::NormalizePathSegments); QString path=url.path(); while (path.endsWith('/')) path.chop(1); url.setPath(path); return url;}
QString origin(QUrl url) {url.setPath({}); return url.toString();}
QString tokenFor(QUrl url,const QVector<OcrService> &known) {
    for (const auto &s:known) if (normalized(s.url)==normalized(url) && !s.token.isEmpty()) return s.token;
    for (const auto &s:known) if (origin(s.url)==origin(url) && !s.token.isEmpty()) return s.token;
    return {};
}
QStringList tasksFromJson(const QJsonValue &value) {
    QStringList result;
    for (const auto &task : value.toArray()) if ((task == "ocr" || task == "formula") && !result.contains(task.toString())) result.append(task.toString());
    return result;
}
}
QStringList ocrProviders() {return {"tesseract","paddleocr","doctr","rapidocr","surya","kraken","native","paddleocr-vl","rapid-latex-ocr","layout"};}
bool localOcrProvider(const QString &provider) {return provider=="tesseract" || provider=="rapid-latex-ocr";}
bool OcrService::supportsFormulas() const {return provider=="rapid-latex-ocr" || provider=="surya" || provider=="paddleocr-vl" || tasks.contains("formula");}
bool OcrService::supportsText() const {return provider!="rapid-latex-ocr" && provider!="layout" && (tasks.isEmpty() || tasks.contains("ocr"));}
OcrService localFormulaService() {
    OcrService s; s.provider="rapid-latex-ocr"; s.label=ocrProviderLabel(s.provider); s.language="auto"; s.tasks={"formula"}; s.ready=!MathOcrEngine::pythonPath().isEmpty(); return s;
}
QString ocrProviderLabel(const QString &provider) {
    if (provider=="tesseract") return "Tesseract (this computer)";
    if (provider=="paddleocr") return "PaddleOCR";
    if (provider=="doctr") return "docTR";
    if (provider=="rapidocr") return "RapidOCR";
    if (provider=="surya") return "Surya";
    if (provider=="kraken") return "Kraken";
    if (provider=="native") return "Native OCR model";
    if (provider=="paddleocr-vl") return "PaddleOCR-VL (vision server)";
    if (provider=="rapid-latex-ocr") return "Local formula model (Rapid LaTeX OCR)";
    if (provider=="layout") return "AI page layout";
    return provider;
}
bool validOcrServiceUrl(const QUrl &url) {
    return url.isValid() && url.toString().size()<=2048 && (url.scheme()=="http" || url.scheme()=="https") &&
        !url.host().isEmpty() && url.userInfo().isEmpty() && !url.hasQuery() && !url.hasFragment();
}
QString OcrService::key() const {
    if (provider=="tesseract") return "local-tesseract";
    if (provider=="rapid-latex-ocr") return "local-formulas";
    return QString::fromLatin1(QCryptographicHash::hash((provider+'\n'+normalized(url).toString(QUrl::FullyEncoded)+(provider=="paddleocr-vl" ? '\n'+model : QString())).toUtf8(),QCryptographicHash::Sha256).toHex());
}
QJsonArray ocrServicesJson(const QVector<OcrService> &services) {
    QJsonArray result;
    for (const auto &s:services) result.append(QJsonObject{{"provider",s.provider},{"label",s.label},{"url",s.url.toString()},{"language",s.language},{"enabled",s.enabled},{"model",s.model},{"tasks",QJsonArray::fromStringList(s.tasks)}});
    return result;
}
QVector<OcrService> readOcrServices(const QJsonValue &json) {
    QVector<OcrService> result; QSet<QString> keys;
    if (!json.isArray() || json.toArray().size()>64) return result;
    for (const auto &v:json.toArray()) {
        const auto o=v.toObject(); OcrService s;
        s.provider=o["provider"].toString(); s.label=o["label"].toString(); s.language=o["language"].toString(); s.url=QUrl(o["url"].toString()); s.enabled=o["enabled"].toBool();
        s.model=o["model"].toString(); s.tasks=tasksFromJson(o["tasks"]);
        if (!v.isObject() || !ocrProviders().contains(s.provider) || s.label.isEmpty() || s.label.size()>128 || s.language.size()>64 ||
            s.model.size()>256 || !o["enabled"].isBool() || !o["url"].isString() || (!localOcrProvider(s.provider) && !validOcrServiceUrl(s.url)) || keys.contains(s.key())) continue;
        if (s.provider=="tesseract") {s.url=QUrl{}; s.ready=!QStandardPaths::findExecutable("tesseract").isEmpty();}
        if (s.provider=="rapid-latex-ocr") s=localFormulaService();
        keys.insert(s.key()); result.append(std::move(s));
    }
    return result;
}
QVector<QUrl> ocrDiscoveryTargets(const QString &hosts,const QVector<OcrService> &known) {
    QVector<QUrl> result;
    const auto add=[&](QUrl url) {url=normalized(url); if (result.size()<32 && validOcrServiceUrl(url) && !result.contains(url)) result.append(url);};
    for (const auto &host:hosts.split(QRegularExpression("[\\s,;]+"),Qt::SkipEmptyParts)) {
        if (host.contains("://")) {add(QUrl(host)); continue;}
        const QUrl base("http://"+host);
        if (base.port()!=-1) add(base);
        else for (int port:{8765,8766,8767,8768,8769,8771,8772,8082}) {auto url=base; url.setPort(port); add(url);}
    }
    for (const auto &s:known) if (!localOcrProvider(s.provider)) add(s.url);
    return result;
}
std::expected<OcrDiscovery,OcrError> discoverOcrServices(const QVector<QUrl> &targets,const QVector<OcrService> &known,std::stop_token stop) {
    if (stop.stop_requested()) return std::unexpected(OcrError{{},true});
    struct Probe {QUrl base; QNetworkReply *reply; QByteArray bytes; bool models=false, oversized=false;};
    std::vector<std::unique_ptr<Probe>> probes;
    QNetworkAccessManager manager; QEventLoop loop; QTimer timer; QElapsedTimer elapsed;
    int finished=0, endpoints=0;
    for (const auto &base:targets) {
        if (endpoints>=32 || !validOcrServiceUrl(base)) continue;
        ++endpoints;
        for (bool models : {false,true}) {
            QUrl url=normalized(base);
            url.setPath(url.path()+(url.path().endsWith("/v1") ? "/" : "/v1/")+(models ? "models" : "health"));
            QNetworkRequest request(url);
            request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,QNetworkRequest::ManualRedirectPolicy);
            const auto token=tokenFor(base,known); if (!token.isEmpty()) request.setRawHeader("Authorization",("Bearer "+token).toUtf8());
            auto p=std::make_unique<Probe>(); p->base=normalized(base); p->models=models; p->reply=manager.get(request); p->reply->setReadBufferSize(256*1024+1);
            auto *probe=p.get(); probes.push_back(std::move(p));
            QObject::connect(probe->reply,&QNetworkReply::readyRead,&loop,[probe] {
                probe->bytes+=probe->reply->readAll(); if (probe->bytes.size()>256*1024) {probe->oversized=true; probe->reply->abort();}
            });
            QObject::connect(probe->reply,&QNetworkReply::finished,&loop,[&] {if (++finished==int(probes.size())) loop.quit();});
        }
    }
    if (!probes.empty()) {
        elapsed.start(); timer.setInterval(40);
        QObject::connect(&timer,&QTimer::timeout,&loop,[&] {if (stop.stop_requested() || elapsed.elapsed()>3000) for (auto &p:probes) if (!p->reply->isFinished()) p->reply->abort();});
        timer.start(); loop.exec();
    }
    if (stop.stop_requested()) return std::unexpected(OcrError{{},true});
    OcrDiscovery result; QSet<QString> keys;
    const auto add=[&](OcrService s) {
        if (keys.contains(s.key()) || result.services.size()>=64) return;
        const auto existing=std::ranges::find_if(known,[&](const auto &k){return k.key()==s.key();});
        if (existing!=known.end()) {s.enabled=existing->enabled; s.token=existing->token; s.label=existing->label; if (!existing->language.isEmpty()) s.language=existing->language;}
        keys.insert(s.key()); result.services.append(std::move(s));
    };
    QSet<QString> compatible; QHash<QString,QString> failures;
    for (auto &p:probes) {
        if (p->reply->isOpen()) p->bytes+=p->reply->readAll();
        const auto root=QJsonDocument::fromJson(p->bytes).object();
        const auto endpoint=p->base.toString();
        if (p->models) {
            if (p->oversized || p->reply->error()!=QNetworkReply::NoError) continue;
            for (const auto &value : root["data"].toArray()) {
                const auto model=value.toObject()["id"].toString();
                if (model.isEmpty() || model.size()>256 || !model.contains("paddleocr-vl",Qt::CaseInsensitive)) continue;
                OcrService s; s.provider="paddleocr-vl"; s.url=p->base; s.model=model; s.label=model;
                s.language="auto"; s.tasks={"ocr","formula"}; s.ready=true; s.token=tokenFor(p->base,known);
                add(std::move(s)); compatible.insert(endpoint);
            }
            continue;
        }
        const QString provider=root["engine_id"].toString();
        if (!p->oversized && p->reply->error()==QNetworkReply::NoError && root["protocol"]=="yagf-layout/1") {
            OcrService s; s.provider="layout"; s.url=p->base; s.model=root["model"].toString().left(128);
            s.label=s.model.isEmpty() ? ocrProviderLabel(s.provider) : s.model;
            s.tasks={"layout"}; s.ready=root["status"]=="ready"; s.token=tokenFor(s.url,known);
            add(std::move(s)); compatible.insert(endpoint); continue;
        }
        if (p->oversized || p->reply->error()!=QNetworkReply::NoError || root["protocol"]!="yagf-ocr/1" || !ocrProviders().contains(provider) || provider=="tesseract") {
            failures.insert(endpoint,p->reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()==401 ? QStringLiteral("token required") : QStringLiteral("no compatible OCR health or PaddleOCR-VL model response")); continue;
        }
        compatible.insert(endpoint);
        if (provider=="native" && root["services"].isArray()) {
            for (const auto &v:root["services"].toArray()) {
                const auto entry=v.toObject(); const QString path=entry["path"].toString(), kind=entry["provider"].toString();
                const QUrl relative(path);
                if (!path.startsWith('/') || path.startsWith("//") || path.size()>512 || relative.hasQuery() || relative.hasFragment() || path.split('/').contains("..") || !ocrProviders().contains(kind) || kind=="tesseract") continue;
                auto url=p->base; url.setPath(url.path()+path);
                OcrService s; s.provider=kind; s.url=url; s.label=entry["label"].toString().left(128); s.language=entry["language"].toString("auto").left(64); s.ready=entry["ready"].toBool(); s.error=entry["reason"].toString().left(2048); s.token=tokenFor(p->base,known);
                s.tasks=tasksFromJson(entry["tasks"]); s.model=entry["model"].toString().left(256);
                if (s.label.isEmpty()) s.label=ocrProviderLabel(kind); add(std::move(s));
            }
        } else {
            OcrService s; s.provider=provider; s.url=p->base; s.label=root["model"].toString(ocrProviderLabel(provider)).left(128); s.language=root["language"].toString("auto").left(64); s.ready=root["status"]=="ready"; s.error=root["reason"].toString().left(2048); s.token=tokenFor(s.url,known); s.tasks=tasksFromJson(root["tasks"]); add(std::move(s));
        }
    }
    for (auto it=failures.cbegin();it!=failures.cend();++it) if (!compatible.contains(it.key())) result.diagnostics.append(it.key()+": "+it.value());
    return result;
}
std::expected<std::unique_ptr<IOcrEngine>,QString> makeOcrServiceEngine(const OcrService &s) {
    if (s.provider=="tesseract") return std::make_unique<TesseractEngine>();
    if (s.provider=="rapid-latex-ocr") return std::make_unique<MathOcrEngine>();
    if (s.provider=="paddleocr-vl" && (s.model.isEmpty() || s.model.size()>256)) return std::unexpected(QStringLiteral("Discover the PaddleOCR-VL model or enter its model name."));
    if (!validOcrServiceUrl(s.url)) return std::unexpected(QStringLiteral("Enter an HTTP(S) service URL without credentials, query or fragment."));
    using P=RemoteOcrEngine::Provider;
    const QStringList names{"paddleocr","doctr","rapidocr","surya","kraken","native","paddleocr-vl"};
    const int index=names.indexOf(s.provider); if (index<0) return std::unexpected(QStringLiteral("Unsupported OCR provider."));
    return std::make_unique<RemoteOcrEngine>(normalized(s.url),s.token,static_cast<P>(index),s.model,s.supportsFormulas());
}
}

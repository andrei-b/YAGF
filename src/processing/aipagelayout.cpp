#include "aipagelayout.h"
#include "formulagrowth.h"
#include <QBuffer>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSet>
#include <QTimer>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <limits>

namespace yagf::processing {
namespace {
auto failure(const QString &message) { return std::unexpected(OcrError{message}); }
bool integer(const QJsonValue &value, int low, int high) {
    const double n=value.toDouble(-1);
    return value.isDouble() && std::isfinite(n) && std::floor(n)==n && n>=low && n<=high;
}
}
std::expected<PageLayout, OcrError> detectPageAreas(const QImage &image, const PageLayoutOptions &options, std::stop_token stop, QJsonObject *evidence)
{
    if (stop.stop_requested()) return std::unexpected(OcrError{QStringLiteral("Cancelled"),true});
    if (image.isNull() || image.width()>20000 || image.height()>20000 || qint64(image.width())*image.height()>80000000)
        return failure(QStringLiteral("The page is empty or exceeds the layout image limit."));
    QUrl url=options.url;
    if (!url.isValid() || (url.scheme()!="http" && url.scheme()!="https") || url.host().isEmpty() ||
        !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment())
        return failure(QStringLiteral("Enter an HTTP or HTTPS layout service URL without credentials, query or fragment."));
    if (!std::isfinite(options.threshold) || options.threshold<.1 || options.threshold>.95)
        return failure(QStringLiteral("Invalid model-score threshold."));
    QString path=url.path();
    while (path.endsWith('/')) path.chop(1);
    if (!path.endsWith("/v1/layout")) path += path.endsWith("/v1") ? "/layout" : "/v1/layout";
    url.setPath(path);
    QByteArray png; QBuffer buffer(&png); buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer,"PNG") || png.size()>32*1024*1024)
        return failure(QStringLiteral("The page PNG exceeds the 32 MiB layout limit."));
    const QByteArray body=QJsonDocument(QJsonObject{{"protocol","yagf-layout/1"},{"image_png",QString::fromLatin1(png.toBase64())},
                                                   {"threshold",options.threshold}}).toJson(QJsonDocument::Compact);
    if (stop.stop_requested()) return std::unexpected(OcrError{QStringLiteral("Cancelled"),true});
    QNetworkAccessManager manager;
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader,"application/json");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,QNetworkRequest::ManualRedirectPolicy);
    if (!options.token.isEmpty()) request.setRawHeader("Authorization",("Bearer "+options.token).toUtf8());
    auto *reply=manager.post(request,body);
    QEventLoop loop; QTimer timer; QElapsedTimer elapsed; elapsed.start();
    QByteArray bytes; bool oversized=false, timedOut=false;
    const auto read=[&] {
        if (!reply->isOpen()) return;
        bytes+=reply->readAll();
        if (bytes.size()>8*1024*1024) { oversized=true; reply->abort(); }
    };
    QObject::connect(reply,&QNetworkReply::readyRead,&loop,read);
    QObject::connect(reply,&QNetworkReply::finished,&loop,&QEventLoop::quit);
    QObject::connect(&timer,&QTimer::timeout,&loop,[&] {
        if (stop.stop_requested() || elapsed.elapsed()>options.timeoutMs) {
            timedOut=!stop.stop_requested(); reply->abort();
        }
    });
    timer.start(40); loop.exec(); read();
    if(evidence) {
        const auto json=QJsonDocument::fromJson(bytes);
        if(json.isObject())*evidence=json.object();
        else *evidence=QJsonObject{{"raw_response_base64",QString::fromLatin1(bytes.toBase64())}};
    }
    if (stop.stop_requested()) return std::unexpected(OcrError{QStringLiteral("Cancelled"),true});
    if (oversized) return failure(QStringLiteral("The layout service response is too large."));
    if (timedOut) return failure(QStringLiteral("The layout service timed out."));
    const int status=reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->error()!=QNetworkReply::NoError || status!=200)
        return failure(QStringLiteral("Could not analyze page areas (%1). Check the layout worker and SSH tunnel. %2")
                           .arg(status).arg(reply->errorString()));
    QJsonParseError parse;
    auto json=QJsonDocument::fromJson(bytes,&parse);
    if (parse.error!=QJsonParseError::NoError || !json.isObject()) return failure(QStringLiteral("Invalid layout JSON."));
    const auto root=json.object();
    if (root.value("protocol")!="yagf-layout/1" || !integer(root.value("width"),image.width(),image.width()) ||
        !integer(root.value("height"),image.height(),image.height()) || !root.value("areas").isArray() ||
        !root.value("model").isString() || root.value("model").toString().isEmpty() || root.value("model").toString().size()>128)
        return failure(QStringLiteral("The layout service returned incompatible geometry or schema."));
    const auto areas=root.value("areas").toArray();
    if (areas.size()>1000) return failure(QStringLiteral("Too many page areas."));
    PageLayout result{root.value("model").toString(),{}}; QSet<QString> ids;
    for (const auto &value:areas) {
        const auto item=value.toObject(); const auto box=item.value("box").toArray();
        const QString id=item.value("id").toString(), label=item.value("label").toString(), type=item.value("type").toString();
        const double score=item.value("score").toDouble(-1);
        if (!value.isObject() || id.isEmpty() || id.size()>128 || ids.contains(id) || label.isEmpty() || label.size()>64 ||
            !(type=="text" || type=="math" || type=="mixed" || type=="picture") || !std::isfinite(score) || score<0 || score>1 ||
            box.size()!=4 || !integer(box[0],0,image.width()-1) || !integer(box[1],0,image.height()-1) ||
            !integer(box[2],1,image.width()) || !integer(box[3],1,image.height()))
            return failure(QStringLiteral("The layout service returned an invalid area."));
        QRect bounds(box[0].toInt(),box[1].toInt(),box[2].toInt(),box[3].toInt());
        if (!image.rect().contains(bounds)) return failure(QStringLiteral("A detected area lies outside the page."));
        PageArea area{label,type,bounds,score,{},type};
        area.id=id;
        if (item.contains("alternatives")) {
            if (!item.value("alternatives").isArray() || item.value("alternatives").toArray().size()>3)
                return failure(QStringLiteral("Invalid alternative area classifications."));
            QSet<QString> types{type};
            for (const auto &value:item.value("alternatives").toArray()) {
                const auto alternative=value.toObject();
                const QString alternativeType=alternative.value("type").toString(), alternativeLabel=alternative.value("label").toString();
                const double alternativeScore=alternative.value("score").toDouble(-1);
                if (!value.isObject() || alternativeLabel.isEmpty() || alternativeLabel.size()>64 ||
                    !(alternativeType=="text" || alternativeType=="picture" || alternativeType=="math" || alternativeType=="mixed") ||
                    types.contains(alternativeType) || !std::isfinite(alternativeScore) || alternativeScore<.4 ||
                    alternativeScore>score+1e-6 || score-alternativeScore>.2+1e-6)
                    return failure(QStringLiteral("Invalid alternative area classification."));
                types.insert(alternativeType); area.alternatives.append({alternativeLabel,alternativeType,alternativeScore});
            }
            std::stable_sort(area.alternatives.begin(),area.alternatives.end(),[](const auto &a,const auto &b){return a.score>b.score;});
        }
        ids.insert(id); result.areas.append(std::move(area));
    }
    result.rawOutput=root;
    result.rawAreas=result.areas;
    consolidateInlineAreas(result.areas);
    return result;
}
void consolidateInlineAreas(QVector<PageArea> &areas)
{
    QSet<int> remove;
    for (int i=0;i<areas.size();++i) if (areas[i].label=="inline_formula") {
        int best=-1; qint64 size=std::numeric_limits<qint64>::max();
        for (int j=0;j<areas.size();++j) if (i!=j && areas[j].label!="inline_formula" &&
            (areas[j].type=="text" || areas[j].type=="mixed") && areas[j].bounds.contains(areas[i].bounds)) {
            const qint64 area=qint64(areas[j].bounds.width())*areas[j].bounds.height();
            if (area<size) { size=area; best=j; }
        }
        if (best>=0) {
            areas[best].type=areas[best].detectedType="mixed";
            areas[best].alternatives.removeIf([](const auto &a){return a.type=="mixed";});
            remove.insert(i);
        }
    }
    for (int i=areas.size()-1;i>=0;--i) if (remove.contains(i)) areas.removeAt(i);
}
void subdivideInlineAreas(QVector<PageArea> &areas, const QImage &gray, const QVector<QRect> &localHints)
{
    if (areas.isEmpty()) return;
    const auto inlineLabel = [](const PageArea &area) { return area.label == "inline_formula"; };
    QVector<int> parents;
    for (int i = 0; i < areas.size(); ++i)
        if (!inlineLabel(areas[i]) && (areas[i].type == "text" || areas[i].type == "mixed")) parents.append(i);
    // Some layout workers return just the inline detections on a tightly cropped paragraph.
    const bool implicitParent = std::ranges::all_of(areas,inlineLabel);
    if (implicitParent) {
        parents.append(areas.size());
        areas.append({"selected_block","text",gray.rect(),-1,{},"text","selected-block"});
    }
    std::stable_sort(parents.begin(),parents.end(),[&](int a,int b){return areas[a].score > areas[b].score;});
    QSet<int> replaced;
    QHash<int,QVector<PageArea>> subdivisions;
    for (int index : parents) {
        if (replaced.contains(index)) continue;
        const auto &parent = areas[index];
        QVector<QRect> hints;
        QVector<int> sources;
        for (int j = 0; j < areas.size(); ++j) {
            if (j != index && !replaced.contains(j) && (inlineLabel(areas[j]) || areas[j].type == "math") &&
                parent.bounds.contains(areas[j].bounds)) {
                hints.append(areas[j].bounds);
                sources.append(j);
            }
        }
        for (const auto &hint : localHints)
            if (parent.bounds.contains(hint)) { hints.append(hint); sources.append(-1); }
        const auto parts = partitionInlineFormulas(gray,parent.bounds,hints);
        if (parts.isEmpty()) continue;
        // Preserve other independent model proposals (in particular figures).
        if (std::ranges::any_of(areas,[&](const PageArea &area) {
                return area.type == "picture" && parent.bounds.intersects(area.bounds);
            })) continue;
        QSet<int> usedHints;
        for (const auto &part : parts) if (part.formula >= 0) usedHints.insert(part.formula);
        QVector<PageArea> children;
        for (const auto &part : parts) {
            const int source = part.formula < 0 ? -1 : sources[part.formula];
            PageArea child = source < 0 ? parent : areas[source];
            child.type = child.detectedType = part.formula < 0 ? "text" : "math";
            child.bounds = part.bounds;
            child.alternatives.clear();
            if (source < 0) {
                child.label = part.formula < 0 ? "text_fragment" : "inline_formula_geometry";
                if (part.formula < 0) {
                    // An expression in a denser line may have no safe gutter.
                    // Keep mixed OCR for that fragment when another line splits.
                    for (int k = 0; k < hints.size(); ++k)
                        if (!usedHints.contains(k) && part.bounds.intersects(hints[k]))
                            child.type = child.detectedType = "mixed";
                }
                child.id = QString("split/%1/%2/%3/%4/%5").arg(parent.id).arg(part.bounds.x()).arg(part.bounds.y())
                    .arg(part.bounds.width()).arg(part.bounds.height());
                child.score = -1; // Raster geometry has no independent model confidence.
            }
            children.append(std::move(child));
        }
        for (int j = 0; j < areas.size(); ++j)
            if (parent.bounds.contains(areas[j].bounds)) replaced.insert(j);
        subdivisions.insert(index,std::move(children));
    }
    QVector<PageArea> result;
    for (int i = 0; i < areas.size(); ++i) {
        if (subdivisions.contains(i)) result += subdivisions.value(i);
        else if (!replaced.contains(i) && !(implicitParent && i == areas.size()-1)) result.append(areas[i]);
    }
    if (result.size() <= 1000) areas = std::move(result);
    else if (implicitParent) areas.removeLast();
}
QVector<core::ImageRegion> mergePageAreas(const QVector<core::ImageRegion> &existing, const QVector<PageArea> &accepted, bool replaceAutomatic)
{
    QVector<core::ImageRegion> result;
    for (const auto &region:existing) if (!replaceAutomatic || !region.autoDetected) result.append(region);
    for (const auto &area:accepted) {
        if (area.bounds.isEmpty() || std::ranges::any_of(result,[&](const auto &r){ return r.bounds.intersects(area.bounds); })) continue;
        core::ImageRegion region;
        region.id=QUuid::createUuid().toString(QUuid::WithoutBraces);
        region.bounds=area.bounds; region.content=area.type; region.notText=area.type=="picture";
        region.autoDetected=true; region.detectedLabel=area.label; region.detectionScore=area.score;
        for (const auto &alternative:area.alternatives) if (alternative.type==area.type) {
            region.detectedLabel=alternative.label; region.detectionScore=alternative.score; break;
        }
        result.append(std::move(region));
    }
    return result;
}
}

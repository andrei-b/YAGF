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
    const auto gray=image.convertToFormat(QImage::Format_Grayscale8);
    consolidateInlineAreas(result.areas,gray);
    refinePageAreaShapes(result.areas,gray);
    return result;
}
namespace {
QVector<PageArea> splitParagraph(const PageArea &parent, const QImage &gray, const QVector<PageArea> &areas,
                                 const QVector<int> &formulaCandidates, const QVector<QRect> &localHints)
{
    QVector<QRect> hints;
    QVector<int> modelSources;
    for (int index : formulaCandidates) {
        if (parent.bounds.contains(areas[index].bounds)) {
            hints.append(areas[index].bounds);
            modelSources.append(index);
        }
    }
    for (const QRect &hint : localHints)
        if (parent.bounds.contains(hint)) hints.append(hint);

    QVector<PageArea> children;
    for (const auto &part : partitionInlineFormulas(gray, parent.bounds, hints)) {
        PageArea child;
        if (part.formula >= 0 && part.formula < modelSources.size()) {
            child = areas[modelSources[part.formula]];
            child.alternatives.clear();
        } else {
            child.label = part.formula < 0 ? "text_fragment" : "inline_formula_geometry";
            child.id = QString("split/%1/%2/%3/%4/%5").arg(parent.id).arg(part.bounds.x()).arg(part.bounds.y())
                .arg(part.bounds.width()).arg(part.bounds.height());
            child.score = -1; // Raster geometry has no independent model confidence.
        }
        child.type = child.detectedType = part.content();
        child.bounds = part.bounds;
        child.splitParentScore = parent.score;
        children.append(std::move(child));
    }
    return children;
}
} // namespace

void consolidateInlineAreas(QVector<PageArea> &areas, const QImage &gray)
{
    QHash<int,QVector<int>> children;
    for (int i = 0; i < areas.size(); ++i) {
        if (areas[i].label != "inline_formula") continue;
        int best = -1;
        qint64 size = std::numeric_limits<qint64>::max();
        for (int j = 0; j < areas.size(); ++j) {
            const auto &parent = areas[j];
            if (parent.label == "inline_formula" || (parent.type != "text" && parent.type != "mixed") ||
                !parent.bounds.contains(areas[i].bounds)) continue;
            const qint64 pixels = qint64(parent.bounds.width()) * parent.bounds.height();
            if (pixels < size) { best = j; size = pixels; }
        }
        if (best >= 0) children[best].append(i);
    }
    QSet<int> consumed;
    QHash<int,QVector<PageArea>> splits;
    qsizetype count = areas.size();
    for (int i = 0; i < areas.size(); ++i) {
        const auto it = children.constFind(i);
        if (it == children.cend()) continue;
        auto &parent = areas[i];
        parent.type = parent.detectedType = "mixed";
        parent.alternatives.removeIf([](const auto &a){return a.type=="mixed";});
        const auto &hints = it.value();
        for (int hint : hints) consumed.insert(hint);
        count -= hints.size();
        // A baseline split can put a printed integral's limits on separate
        // rows. Retain its model crop when there are clear gutters around it.
        bool hasLimits = false;
        for (int hint : hints) {
            auto &box = areas[hint].bounds;
            for (const auto &sign : boundedIntegralSigns(gray, box)) {
                if (!parent.bounds.contains(sign)) continue;
                box |= sign;
                hasLimits = true;
            }
        }
        if (hasLimits) {
            auto ranked = hints;
            std::stable_sort(ranked.begin(), ranked.end(), [&](int a,int b){return areas[a].score > areas[b].score;});
            auto parts = splitParagraph(parent,gray,areas,ranked,{});
            if (!parts.isEmpty() && count + parts.size() - 1 <= 1000) {
                // Continue mixed OCR on the prose fragments: another expression
                // on this line may have fallen below the layout score threshold.
                for (auto &part : parts)
                    if (part.type == "text") part.type = part.detectedType = "mixed";
                count += parts.size() - 1;
                splits.insert(i,std::move(parts));
            }
        }
    }
    QVector<PageArea> result;
    for (int i = 0; i < areas.size(); ++i) {
        if (splits.contains(i)) result += splits.value(i);
        else if (!consumed.contains(i)) result.append(areas[i]);
    }
    areas = std::move(result);
}

void subdivideInlineAreas(QVector<PageArea> &areas, const QImage &gray, const QVector<QRect> &localHints)
{
    if (areas.isEmpty()) return;
    QVector<int> parents, formulas;
    QVector<QRect> pictures;
    bool onlyInline = true;
    for (int i = 0; i < areas.size(); ++i) {
        const auto &area = areas[i];
        const bool isInline = area.label == "inline_formula";
        onlyInline &= isInline;
        if (isInline || area.type == "math") formulas.append(i);
        else if (area.type == "text" || area.type == "mixed") parents.append(i);
        else if (area.type == "picture") pictures.append(area.bounds);
    }
    // Partitioning accepts the first geometrically compatible hint. Prefer the
    // stronger detection even if the worker serialized a weaker duplicate first.
    const auto byScore = [&](int a, int b) { return areas[a].score > areas[b].score; };
    std::stable_sort(formulas.begin(), formulas.end(), byScore);
    if (onlyInline) {
        const PageArea crop{"selected_block","text",gray.rect(),-1,{},"text","selected-block"};
        auto parts = splitParagraph(crop, gray, areas, formulas, localHints);
        if (!parts.isEmpty() && parts.size() <= 1000) areas = std::move(parts);
        return;
    }

    const core::RegionInk ink(gray);
    std::stable_sort(parents.begin(), parents.end(), byScore);
    QSet<int> replaced;
    auto remainingLocalHints = localHints;
    QHash<int,QVector<PageArea>> subdivisions;
    for (int index : parents) {
        const auto &parent = areas[index];
        if (replaced.contains(index) ||
            std::ranges::any_of(pictures, [&](QRect box){return ink.conflicts(box, parent.bounds);})) continue;
        auto children = splitParagraph(parent, gray, areas, formulas, remainingLocalHints);
        if (children.isEmpty()) continue;
        for (int j = 0; j < areas.size(); ++j)
            if (parent.bounds.contains(areas[j].bounds)) replaced.insert(j);
        // Overlapping paragraph proposals can share whitespace yet contain
        // different formulas. Consume only hints covered by this split.
        formulas.removeIf([&](int i){return replaced.contains(i);});
        remainingLocalHints.removeIf([&](QRect box){return parent.bounds.contains(box);});
        subdivisions.insert(index, std::move(children));
    }
    if (subdivisions.isEmpty()) return;
    QVector<PageArea> result;
    for (int i = 0; i < areas.size(); ++i) {
        const auto split = subdivisions.constFind(i);
        if (split != subdivisions.cend()) result += split.value();
        else if (!replaced.contains(i)) result.append(areas[i]);
        if (result.size() > 1000) return;
    }
    areas = std::move(result);
}
void refinePageAreaShapes(QVector<PageArea> &areas,const QImage &gray)
{
    QVector<core::ImageRegion> blocks;
    for (const auto &area : areas) blocks.append(area.block());
    refineLineShapes(gray,blocks);
    for (int i=0; i<areas.size(); ++i) {
        areas[i].bounds=blocks[i].bounds;
        areas[i].parts=blocks[i].parts;
    }
}

QVector<core::ImageRegion> mergePageAreas(const QVector<core::ImageRegion> &existing, const QVector<PageArea> &accepted, bool replaceAutomatic, const core::RegionInk &ink)
{
    QVector<core::ImageRegion> result;
    for (const auto &region:existing) if (!replaceAutomatic || !region.autoDetected) result.append(region);
    for (const auto &area:accepted) {
        auto region = area.block();
        const auto shape = region.shape();
        if (shape.isEmpty() || std::ranges::any_of(result,[&](const auto &r){return ink.shapeConflicts(r.shape(),shape);})) continue;
        region.id=QUuid::createUuid().toString(QUuid::WithoutBraces);
        for (const auto &alternative:area.alternatives) if (alternative.type==area.type) {
            region.detectedLabel=alternative.label; region.detectionScore=alternative.score; break;
        }
        result.append(std::move(region));
    }
    return result;
}
}

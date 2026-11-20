#include "ocrreadings.h"
#include "wordeditor.h"
#include "formulatext.h"
#include <QSet>
#include <algorithm>
#include <cmath>

namespace yagf::core {
namespace {
QJsonArray box(QRect r) { return {r.x(),r.y(),r.width(),r.height()}; }
bool readBox(const QJsonValue &value, QSize size, QRect &r) {
    const auto a=value.toArray(); if (!value.isArray() || a.size()!=4) return false;
    for (const auto &v:a) if (!v.isDouble() || !std::isfinite(v.toDouble()) || v.toDouble()!=std::floor(v.toDouble()) || v.toDouble()<0 || v.toDouble()>1000000) return false;
    r={a[0].toInt(),a[1].toInt(),a[2].toInt(),a[3].toInt()};
    return !r.isEmpty() && QRect(QPoint{},size).contains(r);
}
bool string(const QJsonObject &o, const char *key, int limit) {
    return o[key].isString() && o[key].toString().size()<=limit;
}
QString join(const QVector<OcrReadingItem> &items) {
    QString result; const OcrReadingItem *previous=nullptr;
    for (const auto &item:items) {
        if (previous) result+=(item.block!=previous->block || item.paragraph!=previous->paragraph || item.line!=previous->line || item.layoutBlock || previous->layoutBlock) ? '\n' : ' ';
        result+=item.text; previous=&item;
    }
    return result;
}
}
QJsonArray ocrReadingsJson(const QVector<OcrReading> &readings) {
    QJsonArray result;
    for (const auto &r:readings) {
        QJsonArray items;
        for (const auto &i:r.items) items.append(QJsonObject{{"box",box(i.bounds)},{"text",i.text},{"block",i.block},{"paragraph",i.paragraph},{"line",i.line},{"layout_block",i.layoutBlock}});
        result.append(QJsonObject{{"source",r.source},{"engine",r.engine},{"label",r.label},{"language",r.language},{"error",r.error},{"area",box(r.area)},{"items",items}});
    }
    return result;
}
std::expected<QVector<OcrReading>,QString> readOcrReadings(const QJsonValue &json,QSize size) {
    const auto fail=[] { return std::unexpected(QStringLiteral("Invalid parallel OCR readings.")); };
    if (!json.isArray() || json.toArray().size()>8) return fail();
    QVector<OcrReading> result; QSet<QString> sources; qsizetype characters=0;
    for (const auto &value:json.toArray()) {
        const auto o=value.toObject(); OcrReading r;
        if (!value.isObject() || !string(o,"source",256) || !string(o,"engine",64) || !string(o,"label",128) ||
            !string(o,"language",64) || !string(o,"error",2048) || o["source"].toString().isEmpty() || o["engine"].toString().isEmpty() ||
            o["label"].toString().isEmpty() || !readBox(o["area"],size,r.area) || !o["items"].isArray() || o["items"].toArray().size()>10000) return fail();
        r.source=o["source"].toString(); r.engine=o["engine"].toString(); r.label=o["label"].toString();
        r.language=o["language"].toString(); r.error=o["error"].toString();
        if (sources.contains(r.source)) return fail(); sources.insert(r.source);
        for (const auto &entry:o["items"].toArray()) {
            const auto item=entry.toObject(); OcrReadingItem i;
            if (!entry.isObject() || !string(item,"text",65536) || item["text"].toString().isEmpty() ||
                !item["layout_block"].isBool() || !readBox(item["box"],size,i.bounds) || !r.area.contains(i.bounds)) return fail();
            for (const auto *key:{"block","paragraph","line"}) {
                const auto n=item[key].toDouble(-1);
                if (!item[key].isDouble() || !std::isfinite(n) || n<0 || n>1000000 || n!=std::floor(n)) return fail();
            }
            i.text=item["text"].toString(); i.block=item["block"].toInt(); i.paragraph=item["paragraph"].toInt(); i.line=item["line"].toInt(); i.layoutBlock=item["layout_block"].toBool();
            characters+=i.text.size(); if (characters>8*1024*1024) return fail(); r.items.append(std::move(i));
        }
        result.append(std::move(r));
    }
    return result;
}
QString ocrReadingText(const OcrReading &reading) { return join(reading.items); }
QVector<QVector<int>> ocrReadingSpans(const OcrPage &page) {
    QVector<QVector<int>> spans;
    for (int i=0;i<page.words.size();++i) {
        const auto &w=page.words[i];
        const auto *p=i ? &page.words[i-1] : nullptr;
        if (!p || w.layoutBlock || p->layoutBlock || w.block!=p->block || w.paragraph!=p->paragraph || w.line!=p->line) spans.append(QVector<int>{});
        spans.last().append(i);
    }
    return spans;
}
QString matchedOcrReading(const OcrPage &page,const QVector<int> &span,const OcrReading &reading) {
    QRect bounds; for (int i:span) {if (i<0 || i>=page.words.size()) return {}; bounds|=page.words[i].bounds;}
    if (bounds.isEmpty()) return {};
    const QRect nearby=bounds.adjusted(-std::max(3,bounds.height()/3),-3,std::max(3,bounds.height()/3),3);
    QVector<OcrReadingItem> items;
    for (const auto &item:reading.items) {
        const auto overlap=nearby.intersected(item.bounds);
        if (!nearby.contains(item.bounds.center()) || qint64(overlap.width())*overlap.height()*2<qint64(item.bounds.width())*item.bounds.height()) continue;
        // Coarse paragraph boxes cannot safely supply an individual line.
        if (item.bounds.height()>bounds.height()*2 && span.size()>0 && !page.words[span.first()].layoutBlock) continue;
        items.append(item);
    }
    return join(items).trimmed();
}
std::expected<OcrPage,QString> acceptOcrReading(const OcrPage &page,const QVector<int> &span,const OcrReading &reading,const QString &text) {
    if (span.isEmpty() || text.trimmed().isEmpty() || text.size()>16384) return std::unexpected(QStringLiteral("Select a matching reading of up to 16384 characters."));
    for (int n=0;n<span.size();++n) if (span[n]<0 || span[n]>=page.words.size() || (n && span[n]!=span[n-1]+1)) return std::unexpected(QStringLiteral("Select consecutive source words."));
    const auto &first=page.words[span.first()];
    const bool structured=(span.size()==1 && first.layoutBlock) || !mathTextSpans(text).isEmpty();
    auto result=structured ? WordEditor::apply(page,span,{{text.trimmed(),first.bold,first.italic,first.fontSizePt,first.fontFamily,true}})
                           : WordEditor::acceptPassage(page,span,text);
    if (result) {
        const auto count=result->words.size()-(page.words.size()-span.size());
        for (int n=span.first();n<span.first()+count;++n) {auto &word=result->words[n]; word.reviewed=true; word.alternative={}; word.readingEngine=reading.label;}
    }
    return result;
}
void translateOcrReadings(QVector<OcrReading> &readings,QPoint offset) {
    for (auto &reading:readings) {reading.area.translate(offset); for (auto &item:reading.items) item.bounds.translate(offset);}
}
QVector<OcrReading> cropOcrReadings(const QVector<OcrReading> &readings,QRect area) {
    QVector<OcrReading> result;
    for (auto reading:readings) {
        reading.area=reading.area.intersected(area); if (reading.area.isEmpty()) continue;
        reading.items.removeIf([&](const auto &i){return !area.contains(i.bounds);});
        reading.area.translate(-area.topLeft()); for (auto &i:reading.items) i.bounds.translate(-area.topLeft());
        result.append(std::move(reading));
    }
    return result;
}
void mergeOcrReadings(QVector<OcrReading> &target,QVector<OcrReading> incoming,QRect area) {
    for (auto &r:target) r.items.removeIf([&](const auto &i){return area.intersects(i.bounds);});
    for (auto &r:incoming) {
        auto found=std::ranges::find(target,r.source,&OcrReading::source);
        if (found==target.end()) target.append(std::move(r));
        else {found->items+=r.items; found->area|=r.area; found->error=r.error; found->language=r.language; found->label=r.label;}
    }
    while (target.size()>8) target.removeFirst();
    target.removeIf([](const auto &r){return r.items.isEmpty() && r.error.isEmpty();});
}
}

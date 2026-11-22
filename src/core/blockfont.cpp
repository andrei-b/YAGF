#include "blockfont.h"
#include "formulatext.h"
#include <QHash>
#include <QMap>
#include <QSet>
#include <algorithm>
#include <tuple>

namespace yagf::core {
namespace {
struct Vote {int value; int weight;};
int median(QVector<Vote> values, int fallback)
{
    if (values.isEmpty()) return fallback;
    std::ranges::sort(values, {}, &Vote::value);
    int total=0; for (const auto &vote:values) total+=vote.weight;
    int count=0;
    for (const auto &vote:values) { count+=vote.weight; if (count*2>=total) return vote.value; }
    return values.last().value;
}
bool manualSize(const OcrWord &word)
{
    return word.fontSizePt>0 && word.sourceFontSizePt>0 && word.fontSizePt!=word.sourceFontSizePt;
}
bool formulaOnly(const OcrWord &word)
{
    QString source=formulaSourceText(word);
    const auto spans=mathTextSpans(source);
    if (spans.isEmpty()) return false;
    for (auto span=spans.crbegin();span!=spans.crend();++span) source.remove(span->start,span->length);
    return std::ranges::none_of(source,[](QChar c){return c.isLetterOrNumber();});
}
// Center containment avoids inheriting a neighboring column's style from a
// slightly overshooting word box. An explicit excluded area wins as well.
int containing(const QVector<ImageRegion> &blocks,const OcrWord &word)
{
    for (int i=0;i<blocks.size();++i) if (blocks[i].shape().contains(word.bounds.center())) return i;
    return -1;
}
}
void normalizeBlockFontSizes(OcrPage &page,const QVector<ImageRegion> &blocks,bool singleBlock,std::stop_token stop)
{
    if (stop.stop_requested()) return;
    QSet<QString> formulas;
    for (const auto &region:page.regions) if (region.type=="equation" || region.type=="formula" || region.isFigure())
        for (const auto &paragraph:region.paragraphs) for (const auto &line:paragraph.lines) for (const auto &id:line.wordIds) formulas.insert(id);
    QHash<QString,QVector<int>> groups;
    for (int i=0;i<page.words.size();++i) {
        if (stop.stop_requested()) return;
        const auto &word=page.words[i];
        if (word.text.trimmed().isEmpty() || formulas.contains(word.id) || page.engineId=="rapid-latex-ocr" || formulaOnly(word)) continue;
        const int supplied=containing(blocks,word);
        const int detected=supplied<0 ? containing(page.imageRegions,word) : -1;
        const auto *block=supplied>=0 ? &blocks[supplied] : detected>=0 ? &page.imageRegions[detected] : nullptr;
        if (block && (block->notText || block->content=="picture" || block->content=="math")) continue;
        const QString key=singleBlock ? QStringLiteral("selection") : supplied>=0 ? QStringLiteral("supplied/%1").arg(supplied) :
                          detected>=0 ? QStringLiteral("detected/%1").arg(detected) : QStringLiteral("ocr/%1").arg(word.block);
        groups[key].append(i);
    }
    for (const auto &indices:groups) {
        if (stop.stop_requested()) return;
        QVector<Vote> points,measuredPoints,pixels,measuredPixels;
        QMap<std::tuple<int,int,int>,int> heights;
        for (int index:indices) {
            const auto &word=page.words[index];
            if (manualSize(word)) continue;
            int letters=0; for (QChar c:word.text) if (c.isLetterOrNumber()) ++letters;
            // Short glyphs and punctuation follow the block; they cannot vote
            // for a smaller body size merely because their ink is shorter.
            if (letters<2) continue;
            const int weight=std::clamp(letters,1,32);
            const int pointSize=word.sourceFontSizePt>0 ? word.sourceFontSizePt : word.fontSizePt;
            if (pointSize>0) points.append({pointSize,weight});
            if (word.sourceFontSizePx>0) {
                pixels.append({word.sourceFontSizePx,weight});
                if (!word.fontFamily.isEmpty()) {
                    measuredPixels.append({word.sourceFontSizePx,weight});
                    if (pointSize>0) measuredPoints.append({pointSize,weight});
                }
            }
            if (!word.layoutBlock && !word.text.contains('\n') && !word.text.contains('\t')) {
                int height=word.bounds.height();
                // A word joined across a line break retains individual source
                // fragments. Its union's height is not a font measurement.
                if (word.sourceBoxes.size()>1) {
                    QVector<Vote> fragments;
                    for (const auto &box:word.sourceBoxes) if (!box.isEmpty()) fragments.append({box.height(),1});
                    height=median(fragments,height);
                }
                auto &line=heights[{word.block,word.paragraph,word.line}]; line=std::max(line,height);
            }
        }
        const int pointSize=std::clamp(median(measuredPoints,median(points,11)),1,72);
        QVector<Vote> lineHeights; for (int height:heights) if (height>0) lineHeights.append({height,1});
        const int fallback=lineHeights.isEmpty() ? qRound(double(pointSize)*page.renderDpi/72.) : qRound(median(lineHeights,20)*1.15);
        const int pixelSize=std::clamp(median(measuredPixels,median(pixels,fallback)),1,4096);
        for (int index:indices) {
            auto &word=page.words[index];
            if (manualSize(word)) continue;
            word.fontSizePt=word.sourceFontSizePt=pointSize;
            word.sourceFontSizePx=pixelSize;
        }
    }
}
}

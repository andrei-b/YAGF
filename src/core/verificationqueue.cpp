#include "verificationqueue.h"
#include "wordspelling.h"
#include "formulatext.h"
#include <QSet>
#include <algorithm>
#include <limits>

namespace yagf::core {
namespace {
QVector<QRect> fragments(const OcrWord &word)
{
    return word.sourceBoxes.isEmpty() ? QVector<QRect>{word.bounds} : word.sourceBoxes;
}
bool collides(const OcrWord &a, const OcrWord &b)
{
    if (!a.bounds.intersects(b.bounds)) return false;
    for (const auto &x : fragments(a)) for (const auto &y : fragments(b)) {
        const QRect common = x.intersected(y);
        const double smaller = std::min(double(x.width()) * x.height(), double(y.width()) * y.height());
        if (smaller > 0 && double(common.width()) * common.height() / smaller > .4) return true;
    }
    return false;
}

// Image blocks are editable crop targets. Words outside them still belong to
// an OCR hierarchy block, including in older projects with no crop outlines.
void indexPageTargets(const OcrPage &page, const QHash<QString,unsigned> &issues, VerificationQueue &queue)
{
    struct Block {
        VerificationItem item;
        QRegion shape;
        bool formula = false, hasText = false, hasFormula = false;
        QString excerpt;
    };
    QVector<Block> blocks;
    QHash<QString,int> hierarchy;
    QSet<QString> equationWords;
    QSet<QString> singleWordEquations;
    const auto appendBlock = [&](NavigationTarget target, const QString &id, QRect bounds,
                                 const QString &label, bool formula, QRegion shape) {
        VerificationItem item{page.number, {}, {}, bounds};
        item.target = target; item.blockId = id; item.label = label;
        blocks.append({item,std::move(shape),formula});
    };
    for (int i=0; i<page.imageRegions.size(); ++i) {
        const auto &region=page.imageRegions[i];
        if (region.notText || region.content=="picture") continue;
        appendBlock(NavigationTarget::ImageBlock,region.id,region.bounds,
                    QStringLiteral("Block %1").arg(i+1),
                    region.content=="math",region.shape());
    }
    const int imageCount = blocks.size();
    for (const auto &region : page.regions) {
        const bool equation = region.type=="formula" || region.type=="equation";
        for (const auto &p : region.paragraphs) for (const auto &line : p.lines)
            for (const auto &id : line.wordIds) {
                hierarchy.insert(id,blocks.size());
                if (equation) {
                    equationWords.insert(id);
                    if (line.wordIds.size()==1) singleWordEquations.insert(id);
                }
            }
        appendBlock(NavigationTarget::OcrBlock,region.id,region.bounds,
                    QStringLiteral("OCR block %1").arg(blocks.size()-imageCount+1),equation,{});
    }
    const auto imageOwner = [&](const OcrWord &word, bool recordOverlap) {
        int owner = -1;
        qint64 smallest = std::numeric_limits<qint64>::max();
        QVector<int> matches;
        const auto containsWord = [&](const QRegion &shape) {
            return word.sourceBoxes.isEmpty() ? shape.contains(word.bounds.center()) :
                std::ranges::any_of(word.sourceBoxes,[&](QRect fragment){return shape.contains(fragment.center());});
        };
        for (int i=0; i<imageCount; ++i) {
            auto &b = blocks[i];
            if (!b.item.bounds.intersects(word.bounds)) continue;
            if (!containsWord(b.shape)) continue;
            if (recordOverlap) {
                b.hasText |= !word.text.trimmed().isEmpty();
                for (int other : matches) if (containsWord(b.shape.intersected(blocks[other].shape))) {
                    b.item.reasons |= SharedWord;
                    blocks[other].item.reasons |= SharedWord;
                }
                matches.append(i);
            }
            const auto size = qint64(b.item.bounds.width())*b.item.bounds.height();
            if (size<smallest) {owner=i; smallest=size;}
        }
        return owner;
    };
    const auto addExcerpt=[](Block &b, const OcrWord &word) {
        b.hasText |= !word.text.trimmed().isEmpty();
        if (b.excerpt.size()>=120) return;
        if (!b.excerpt.isEmpty()) b.excerpt+=' ';
        b.excerpt+=word.text.left(120-b.excerpt.size());
    };
    constexpr unsigned uncertain = LowConfidence | UnknownWord | ConflictingReading | GeometryAnomaly;
    const auto addFormula = [&](VerificationItem &item) {
        item.formulaPreview.clear();
        if (item.text.size() <= 2048) {
            if (const auto preview = unicodeMath(item.text)) item.formulaPreview = *preview;
        }
        queue.formulas.append(item);
    };
    for (const auto &word : page.words) {
        const auto flags = issues.value(word.id);
        VerificationItem item{page.number,word.id,word.text,word.bounds,flags};
        item.reviewed = word.reviewed;
        item.confidence = word.confidence;
        if ((flags & uncertain) && !word.layoutBlock) queue.uncertainWords.append(item);
        const int ocr=hierarchy.value(word.id,-1);
        int owner=imageOwner(word,true);
        if (owner<0) owner=ocr;
        const auto source = formulaSourceText(word);
        const auto spans = mathTextSpans(source);
        bool hasFormula = false;
        for (const auto &span : spans) {
            item.text=span.source;
            item.label=span.display ? QStringLiteral("Display formula") : QStringLiteral("Inline formula");
            item.formulaStart=span.start+(span.length-span.source.size())/2;
            item.formulaLength=span.source.size();
            item.displayFormula=span.display;
            addFormula(item); hasFormula=true;
        }
        if (spans.isEmpty() && !source.trimmed().isEmpty() &&
            (singleWordEquations.contains(word.id) || word.readingEngine=="rapid-latex-ocr" || page.engineId=="rapid-latex-ocr" ||
             (word.layoutBlock && (equationWords.contains(word.id) || (owner>=0 && blocks[owner].formula))))) {
            item.text=source; item.label=QStringLiteral("Formula");
            item.formulaStart=0; item.formulaLength=source.size(); item.displayFormula=true;
            addFormula(item); hasFormula=true;
        }
        if (owner<0) continue;
        auto &b=blocks[owner];
        b.item.reasons |= flags & uncertain;
        addExcerpt(b,word);
        b.hasFormula |= hasFormula;
        if (ocr>=0 && owner!=ocr && blocks[ocr].formula) {
            addExcerpt(blocks[ocr],word);
            blocks[ocr].hasFormula |= hasFormula || b.formula;
        }
    }
    for (const auto &saved : page.pendingCorrections) {
        int owner=imageOwner(saved,false);
        if (owner<0) {
            for (int i=imageCount; i<blocks.size(); ++i)
                if (blocks[i].item.bounds.contains(saved.bounds.center())) {owner=i; break;}
        }
        if (owner>=0) blocks[owner].item.reasons |= PendingCorrection;
    }
    for (auto &b : blocks) {
        if (!b.hasText && b.item.target==NavigationTarget::ImageBlock && !page.engineId.isEmpty())
            b.item.reasons |= EmptyBlock;
        b.item.text=b.excerpt.simplified();
        if (b.item.text.isEmpty()) b.item.text=b.formula ? QStringLiteral("Formula block") : QStringLiteral("Text block");
        if (b.item.reasons) queue.problemBlocks.append(b.item);
        if (b.formula && !b.hasFormula && (b.hasText || b.item.target==NavigationTarget::ImageBlock))
            queue.formulas.append(b.item);
    }
}
}
VerificationQueue buildVerificationQueue(const OcrDocument &document, const SpellingCheck &unknown, double threshold)
{
    VerificationQueue result;
    QHash<int, const OcrPage *> pages;
    for (const auto &page : document.pages) pages.insert(page.number, &page);
    for (int number = 1; number <= document.totalPages; ++number) {
        const OcrPage *page = pages.value(number, nullptr);
        if (!page || page->engineId.isEmpty()) {
            result.items.append({number, {}, QStringLiteral("Page not recognized"), {}, UnrecognizedPage, false});
            if (page) indexPageTargets(*page,{},result);
            continue;
        }
        ++result.recognizedPages;
        const auto spelling = wordSpelling(*page);
        const qsizetype before = result.items.size();
        QHash<QString,unsigned> issues;
        QSet<int> geometry;
        QVector<int> order;
        for (int i = 0; i < page->words.size(); ++i) order.append(i);
        std::sort(order.begin(), order.end(), [&](int a, int b) { return page->words[a].bounds.left() < page->words[b].bounds.left(); });
        const QRect extent(0, 0, page->width, page->height);
        for (int i = 0; i < order.size(); ++i) {
            const auto &a = page->words[order[i]];
            if (a.bounds.isEmpty() || !extent.contains(a.bounds)) geometry.insert(order[i]);
            for (int j = i + 1; j < order.size() && page->words[order[j]].bounds.left() <= a.bounds.right(); ++j)
                if (collides(a, page->words[order[j]])) { geometry.insert(order[i]); geometry.insert(order[j]); }
        }
        for (int i = 0; i < page->words.size(); ++i) {
            const auto &word = page->words[i];
            ++result.totalWords;
            if (word.reviewed) { ++result.verifiedWords; continue; }
            unsigned reasons = 0;
            if ((word.layoutBlock && (word.readingEngine == "rapid-latex-ocr" || isFormulaWord(*page, word) ||
                                     !mathTextSpans(formulaSourceText(word)).isEmpty())) ||
                (word.alternative.formula && !word.alternative.text.isEmpty())) reasons |= MathReading;
            if (word.confidence >= 0 && word.confidence < threshold) reasons |= LowConfidence;
            if (!word.alternative.text.isEmpty() && word.alternative.text.simplified() != word.text.simplified()) reasons |= ConflictingReading;
            if (geometry.contains(i)) reasons |= GeometryAnomaly;
            if (unknown) {
                for (const auto &token : spelling.tokens[i])
                    if (unknown(*page, word, token)) { reasons |= UnknownWord; break; }
            }
            if (reasons) {
                issues.insert(word.id,reasons);
                result.items.append({number, word.id, word.text, word.bounds, reasons, false});
                result.items.last().confidence=word.confidence;
            }
        }
        for (const auto &word : page->pendingCorrections)
            result.items.append({number, word.id, word.text.isEmpty() ? QStringLiteral("Saved deletion") : word.text,
                                 word.bounds, PendingCorrection, true});
        if (result.items.size() == before) ++result.completedPages;
        indexPageTargets(*page,issues,result);
    }
    return result;
}
QString verificationReasons(unsigned reasons)
{
    QStringList names;
    if (reasons & LowConfidence) names << QStringLiteral("Low confidence");
    if (reasons & ConflictingReading) names << QStringLiteral("Conflicting reading");
    if (reasons & UnknownWord) names << QStringLiteral("Unknown word");
    if (reasons & GeometryAnomaly) names << QStringLiteral("Geometry");
    if (reasons & PendingCorrection) names << QStringLiteral("Saved correction");
    if (reasons & UnrecognizedPage) names << QStringLiteral("Not recognized");
    if (reasons & MathReading) names << QStringLiteral("Check formula symbols");
    if (reasons & EmptyBlock) names << QStringLiteral("No OCR text");
    if (reasons & SharedWord) names << QStringLiteral("Shared OCR word");
    return names.join(QStringLiteral(", "));
}
}

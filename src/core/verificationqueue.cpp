#include "verificationqueue.h"
#include "wordspelling.h"
#include <QSet>
#include <algorithm>

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
            continue;
        }
        ++result.recognizedPages;
        const auto spelling = wordSpelling(*page);
        const qsizetype before = result.items.size();
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
            if ((word.layoutBlock && word.readingEngine == "rapid-latex-ocr") ||
                (word.alternative.formula && !word.alternative.text.isEmpty())) reasons |= MathReading;
            if (word.confidence >= 0 && word.confidence < threshold) reasons |= LowConfidence;
            if (!word.alternative.text.isEmpty() && word.alternative.text.simplified() != word.text.simplified()) reasons |= ConflictingReading;
            if (geometry.contains(i)) reasons |= GeometryAnomaly;
            if (unknown) {
                for (const auto &token : spelling.tokens[i])
                    if (unknown(*page, word, token)) { reasons |= UnknownWord; break; }
            }
            if (reasons) result.items.append({number, word.id, word.text, word.bounds, reasons, false});
        }
        for (const auto &word : page->pendingCorrections)
            result.items.append({number, word.id, word.text.isEmpty() ? QStringLiteral("Saved deletion") : word.text,
                                 word.bounds, PendingCorrection, true});
        if (result.items.size() == before) ++result.completedPages;
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
    return names.join(QStringLiteral(", "));
}
}

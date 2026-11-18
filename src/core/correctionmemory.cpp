#include "correctionmemory.h"

#include <algorithm>

namespace yagf::core {
namespace {

bool singleToken(const QString &text)
{
    return !text.isEmpty() && std::none_of(text.begin(), text.end(), [](QChar c) { return c.isSpace(); });
}

QString neighbor(const OcrPage &page, int index, int offset)
{
    const int other = index + offset;
    if (other < 0 || other >= page.words.size()) return {};
    const auto &word = page.words[index];
    const auto &adjacent = page.words[other];
    if (word.block != adjacent.block || word.paragraph != adjacent.paragraph || word.line != adjacent.line)
        return {};
    return adjacent.text;
}

// Restrict approximate matches to longer alphabetic readings. Identifiers containing
// punctuation or digits still participate in exact, case-sensitive matching.
bool fuzzyEligible(const QString &text)
{
    return text.size() >= 5 && text.size() <= 80 &&
        std::all_of(text.begin(), text.end(), [](QChar c) { return c.isLetter(); });
}

bool oneEditApart(QStringView a, QStringView b)
{
    if (a.size() > b.size()) std::swap(a, b);
    if (b.size() - a.size() > 1) return false;
    qsizetype i = 0;
    while (i < a.size() && a[i] == b[i]) ++i;
    if (i == a.size()) return b.size() > a.size();
    return a.sliced(i + (a.size() == b.size())) == b.sliced(i + 1);
}

} // namespace

bool isVerifiedCorrection(const OcrWord &word)
{
    // WordEditor resets confidence on estimated split/merge geometry.
    return !word.layoutBlock && word.reviewed && word.confidence >= 0 && word.originalText != word.text &&
        singleToken(word.originalText) && singleToken(word.text) &&
        word.sourceWordIds.size() == 1 && word.sourceBoxes.size() == 1 &&
        word.bounds.isValid() && word.sourceBoxes.first() == word.bounds;
}

CorrectionMemory::CorrectionMemory(const OcrDocument &document)
{
    for (const auto &page : document.pages) {
        for (int i = 0; i < page.words.size(); ++i) {
            const auto &word = page.words[i];
            if (!isVerifiedCorrection(word)) continue;
            auto &entry = corrections[word.originalText][word.text];
            ++entry.occurrences;
            const QString previous = neighbor(page, i, -1);
            const QString next = neighbor(page, i, 1);
            if (!previous.isEmpty()) entry.previousWords.insert(previous);
            if (!next.isEmpty()) entry.nextWords.insert(next);
        }
    }
}

QVector<CorrectionSuggestion> CorrectionMemory::suggestions(const OcrPage &page, int index) const
{
    if (index < 0 || index >= page.words.size()) return {};
    const OcrWord &word = page.words[index];
    if (word.reviewed || word.text != word.originalText || !singleToken(word.text)) return {};
    const QString previous = neighbor(page, index, -1);
    const QString next = neighbor(page, index, 1);
    QVector<CorrectionSuggestion> result;
    const auto collect = [&](const auto &entry, bool exact) {
        for (auto it = entry.value().cbegin(); it != entry.value().cend(); ++it) {
            if (it.key() == word.text) continue;
            if (!exact && !fuzzyEligible(it.key())) continue;
            const auto &evidence = it.value();
            result.append({entry.key(), it.key(), evidence.occurrences, exact,
                           evidence.previousWords.contains(previous) + evidence.nextWords.contains(next)});
        }
    };
    const auto exact = corrections.constFind(word.text);
    if (exact != corrections.cend()) collect(exact, true);
    else if (fuzzyEligible(word.text)) {
        for (auto it = corrections.cbegin(); it != corrections.cend(); ++it)
            if (fuzzyEligible(it.key()) && oneEditApart(word.text, it.key())) collect(it, false);
    }
    std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) {
        if (a.matchingNeighbors != b.matchingNeighbors) return a.matchingNeighbors > b.matchingNeighbors;
        if (a.occurrences != b.occurrences) return a.occurrences > b.occurrences;
        if (a.replacement != b.replacement) return a.replacement < b.replacement;
        return a.original < b.original;
    });
    QVector<CorrectionSuggestion> distinct;
    QSet<QString> seen;
    for (const auto &item : result) {
        if (seen.contains(item.replacement)) continue;
        seen.insert(item.replacement);
        distinct.append(item);
        if (distinct.size() == 3) break;
    }
    return distinct;
}

} // namespace yagf::core

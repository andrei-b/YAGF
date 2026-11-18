#include "correctionmemory.h"
#include "bookvocabulary.h"

#include <algorithm>

namespace yagf::core {
namespace {

bool singleToken(const QString &text)
{
    return !text.isEmpty() && std::ranges::none_of(text, [](QChar c) { return c.isSpace(); });
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
        std::ranges::all_of(text, [](QChar c) { return c.isLetter(); });
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
    return hasOriginalWordBox(word) && word.reviewed && word.originalText != word.text &&
        singleToken(word.originalText) && singleToken(word.text);
}

CorrectionMemory::CorrectionMemory(const OcrDocument &document, bool matchSimilarSymbols)
{
    for (const auto &page : document.pages) {
        QHash<QByteArray, QHash<QString, bool>> readingsOnPage;
        for (int i = 0; i < page.words.size(); ++i) {
            const auto &word = page.words[i];
            if (word.symbolShape.size() == 32 && hasOriginalWordBox(word) && isSingleSymbol(word.text) &&
                (word.reviewed || (word.confidence >= 95 && word.originalText == word.text))) {
                readingsOnPage[word.symbolShape][word.text] |= word.reviewed;
            }
            if (!isVerifiedCorrection(word)) continue;
            auto &entry = corrections[word.originalText][word.text];
            ++entry.occurrences;
            const QString previous = neighbor(page, i, -1);
            const QString next = neighbor(page, i, 1);
            if (!previous.isEmpty()) entry.previousWords.insert(previous);
            if (!next.isEmpty()) entry.nextWords.insert(next);
        }
        for (auto shape = readingsOnPage.cbegin(); shape != readingsOnPage.cend(); ++shape) {
            for (auto label = shape.value().cbegin(); label != shape.value().cend(); ++label)
                symbolExamples[shape.key()].append({label.key(), page.language, page.number, label.value()});
        }
    }
    if (matchSimilarSymbols) {
        for (auto it = symbolExamples.cbegin(); it != symbolExamples.cend(); ++it)
            similarSymbols.insert(it.key());
    }
}

std::optional<CorrectionSuggestion> CorrectionMemory::symbolSuggestion(const OcrPage &page,
                                                                      const OcrWord &word) const
{
    if (word.confidence >= 85 || !hasOriginalWordBox(word) || !isSingleSymbol(word.text) ||
        word.symbolShape.size() != 32) return {};

    QString reading;
    QSet<int> pages;
    bool confirmed = false;
    const auto agree = [&](const QByteArray &mask) {
        const auto found = symbolExamples.constFind(mask);
        if (found == symbolExamples.cend()) return true;
        for (const auto &sample : found.value()) {
            if (sample.page >= page.number || sample.language != page.language) continue;
            if (!reading.isEmpty() && reading != sample.reading) return false;
            reading = sample.reading;
            confirmed |= sample.reviewed;
            pages.insert(sample.page);
        }
        return true;
    };
    if (!agree(word.symbolShape)) return {};
    SymbolMatch match = SymbolMatch::Exact;
    if (pages.isEmpty()) {
        match = SymbolMatch::Similar;
        for (const auto &mask : similarSymbols.similar(word.symbolShape))
            if (!agree(mask)) return {};
    }
    if (reading.isEmpty() || reading == word.text || (!confirmed && pages.size() < 2)) return {};
    return CorrectionSuggestion{word.text, reading, int(pages.size()), false, 0, match};
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
    if (auto symbol = symbolSuggestion(page, word)) result.append(*symbol);
    std::ranges::sort(result, [](const auto &a, const auto &b) {
        if (a.exact != b.exact) return a.exact;
        if (a.symbolMatch != b.symbolMatch) return a.symbolMatch > b.symbolMatch;
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

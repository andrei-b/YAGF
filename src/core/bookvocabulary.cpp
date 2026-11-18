#include "bookvocabulary.h"

#include <QSet>
#include <algorithm>

namespace yagf::core {

bool hasOriginalWordBox(const OcrWord &word)
{
    // Split/merged and inserted words have estimated geometry and unknown confidence.
    return !word.layoutBlock && word.confidence >= 0 && word.sourceWordIds.size() == 1 &&
        word.sourceBoxes.size() == 1 && word.bounds.isValid() && word.sourceBoxes.first() == word.bounds;
}

bool isSingleSymbol(const QString &text)
{
    return (text.size() == 1 && !text.front().isSurrogate() && text.front().isPrint() && !text.front().isSpace()) ||
           (text.size() == 2 && text.front().isHighSurrogate() && text.back().isLowSurrogate());
}

void BookVocabulary::addPage(const OcrPage &page)
{
    QSet<QString> confidentOnPage;
    for (const auto &word : page.words) {
        if (!hasOriginalWordBox(word) || word.text.isEmpty() || word.text.size() > 64) continue;
        const auto characters = word.text.toUcs4();
        if (std::any_of(characters.begin(), characters.end(), [](char32_t c) {
            return !QChar::isPrint(c) || QChar::isSpace(c);
        })) continue;
        if (word.reviewed) {
            ++entries[word.text].verified;
            if (word.originalText != word.text) entries[word.originalText].correctedAway = true;
        } else if (word.confidence >= 95 && word.text == word.originalText) {
            confidentOnPage.insert(word.text);
        }
    }
    for (const QString &term : confidentOnPage) ++entries[term].confidentPages;
}

QStringList BookVocabulary::terms() const
{
    QStringList candidates;
    for (auto it = entries.cbegin(); it != entries.cend(); ++it) {
        const Evidence &e = it.value();
        if (e.verified || (!e.correctedAway && e.confidentPages >= 2)) candidates.append(it.key());
    }
    const qsizetype count = std::min<qsizetype>(4096, candidates.size());
    std::partial_sort(candidates.begin(), candidates.begin() + count, candidates.end(),
        [this](const QString &a, const QString &b) {
            const Evidence left = entries.value(a), right = entries.value(b);
            if (left.verified != right.verified) return left.verified > right.verified;
            if (left.confidentPages != right.confidentPages) return left.confidentPages > right.confidentPages;
            return a < b;
        });
    candidates.resize(count);
    return candidates;
}

} // namespace yagf::core

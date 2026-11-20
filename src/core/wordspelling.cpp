#include "wordspelling.h"

#include <QRegularExpression>
#include <algorithm>

namespace yagf::core {
WordSpelling wordSpelling(const OcrPage &page)
{
    static const QRegularExpression tokens(QStringLiteral("[\\p{L}\\p{M}\\p{N}_]+(?:['’][\\p{L}\\p{M}]+)*"));
    static const QRegularExpression identifiers(QStringLiteral("[\\p{N}_]"));
    static const QRegularExpression end(QStringLiteral("([\\p{L}\\p{M}]+)[-‐\u00ad]\\s*$"));
    static const QRegularExpression start(QStringLiteral("^\\s*([\\p{L}\\p{M}]+)"));
    static const QRegularExpression hyphen(QStringLiteral("^[-‐\u00ad]$"));
    WordSpelling result;
    result.tokens.resize(page.words.size());
    for (int i = 0; i < page.words.size(); ++i) {
        const auto &word = page.words[i];
        if (word.layoutBlock) continue;
        auto matches = tokens.globalMatch(word.text);
        while (matches.hasNext()) {
            const auto token = matches.next().captured();
            if (!token.contains(identifiers)) result.tokens[i].append(token);
        }
    }
    for (int i = 0; i + 1 < page.words.size(); ++i) {
        const auto &a = page.words[i];
        const auto &b = page.words[i+1];
        if (a.layoutBlock || b.layoutBlock || a.block != b.block || a.paragraph != b.paragraph ||
            b.line != a.line+1 || b.bounds.center().y() <= a.bounds.center().y() ||
            b.bounds.top()-a.bounds.bottom() > 3*std::max(a.bounds.height(), b.bounds.height())) continue;
        int first = i;
        QString leftText = a.text;
        // Tesseract sometimes emits the line-end hyphen as a separate token.
        if (hyphen.match(a.text).hasMatch() && i > 0) {
            const auto &previous = page.words[i-1];
            if (previous.layoutBlock || previous.block != a.block || previous.paragraph != a.paragraph ||
                previous.line != a.line || a.bounds.left() < previous.bounds.right() ||
                a.bounds.left()-previous.bounds.right() > a.bounds.height()) continue;
            first = i-1;
            leftText = previous.text + a.text;
        }
        const auto left = end.match(leftText), right = start.match(b.text);
        if (!left.hasMatch() || !right.hasMatch() || result.tokens[first].isEmpty() || result.tokens[i+1].isEmpty()) continue;
        if (result.tokens[first].last() != left.captured(1) || result.tokens[i+1].first() != right.captured(1)) continue;
        const QString joined = left.captured(1) + right.captured(1);
        result.tokens[first].last() = joined;
        result.tokens[i+1].first() = joined;
        result.continuations.insert(first);
        result.continuations.insert(i);
        result.continuations.insert(i+1);
    }
    return result;
}
}

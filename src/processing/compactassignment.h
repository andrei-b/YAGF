#pragma once

#include "ocrdocument.h"
#include <QRegularExpression>
#include <QSet>
#include <algorithm>

namespace yagf::processing {
inline bool compactAssignment(const QString &text)
{
    // Short variables with indices can look like Cyrillic prose (рз, р3, х1).
    // Require a complete comparison; ordinary words and footnotes stay prose.
    static const QRegularExpression assignment(QStringLiteral(
        "^\\s*[(\\[]?\\s*\\p{L}(?:[\\p{L}\\p{N}]{0,2}|_\\{?[\\p{L}\\p{N}]{1,3}\\}?)\\s*"
        "(?:=|≠|≈|≤|≥|<|>)\\s*(?:[+−-]?\\p{N}+(?:[.,]\\p{N}+)?|\\p{L}[\\p{L}\\p{N}]{0,2})\\s*[),;:.\\]]*\\s*$"));
    return assignment.match(text).hasMatch();
}

inline QSet<int> compactAssignmentWords(const QVector<core::OcrWord> &words)
{
    QSet<int> result;
    for (int first = 0; first < words.size(); ++first) {
        const auto &anchor = words[first];
        QString tokens;
        for (int end = first; end < std::min<qsizetype>(words.size(), first+4); ++end) {
            const auto &word = words[end];
            if (word.layoutBlock || word.reviewed || word.block != anchor.block ||
                word.paragraph != anchor.paragraph || word.line != anchor.line) break;
            if (end > first && (word.bounds.left() < words[end-1].bounds.left() ||
                word.bounds.left()-words[end-1].bounds.right() > 2*std::max(word.bounds.height(), words[end-1].bounds.height()))) break;
            tokens += word.text;
            if (!compactAssignment(tokens)) continue;
            for (int i = first; i <= end; ++i) result.insert(i);
            break;
        }
    }
    return result;
}
}

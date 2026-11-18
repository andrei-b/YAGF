#pragma once

#include "ocrdocument.h"
#include "symbolmatcher.h"

#include <optional>

#include <QHash>
#include <QSet>

namespace yagf::core {

enum class SymbolMatch : quint8 { None, Similar, Exact };

struct CorrectionSuggestion {
    QString original;
    QString replacement;
    int occurrences = 0;
    bool exact = false;
    int matchingNeighbors = 0;
    SymbolMatch symbolMatch = SymbolMatch::None;
};

bool isVerifiedCorrection(const OcrWord &word);

class CorrectionMemory {
public:
    explicit CorrectionMemory(const OcrDocument &document = {}, bool matchSimilarSymbols = false);
    [[nodiscard]] QVector<CorrectionSuggestion> suggestions(const OcrPage &page, int index) const;

private:
    struct Evidence {
        int occurrences = 0;
        QSet<QString> previousWords;
        QSet<QString> nextWords;
    };
    struct SymbolExample {
        QString reading;
        QString language;
        int page;
        bool reviewed;
    };
    [[nodiscard]] std::optional<CorrectionSuggestion> symbolSuggestion(const OcrPage &page, const OcrWord &word) const;
    SymbolMatcher similarSymbols;
    QHash<QByteArray, QVector<SymbolExample>> symbolExamples;
    QHash<QString, QHash<QString, Evidence>> corrections;
};

} // namespace yagf::core

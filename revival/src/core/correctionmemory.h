#pragma once

#include "ocrdocument.h"

#include <QHash>
#include <QSet>

namespace yagf::core {

struct CorrectionSuggestion {
    QString original;
    QString replacement;
    int occurrences = 0;
    bool exact = false;
    int matchingNeighbors = 0;
};

bool isVerifiedCorrection(const OcrWord &word);

class CorrectionMemory {
public:
    explicit CorrectionMemory(const OcrDocument &document = {});
    QVector<CorrectionSuggestion> suggestions(const OcrPage &page, int index) const;

private:
    struct Evidence {
        int occurrences = 0;
        QSet<QString> previousWords;
        QSet<QString> nextWords;
    };
    QHash<QString, QHash<QString, Evidence>> corrections;
};

} // namespace yagf::core

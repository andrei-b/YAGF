#pragma once

#include "ocrdocument.h"
#include <QHash>

namespace yagf::core {

// Accumulate each earlier page once per sequential OCR run and language.
class BookVocabulary {
public:
    void addPage(const OcrPage &page);
    [[nodiscard]] QStringList terms() const;

private:
    struct Evidence {
        int verified = 0;
        int confidentPages = 0;
        bool correctedAway = false;
    };
    QHash<QString, Evidence> entries;
};

bool hasOriginalWordBox(const OcrWord &word);
bool isSingleSymbol(const QString &text);

} // namespace yagf::core

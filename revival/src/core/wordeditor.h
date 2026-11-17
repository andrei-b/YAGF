#pragma once

#include "ocrdocument.h"

#include <expected>

namespace yagf::core {

struct EditedWord {
    QString text;
    bool bold = false;
    bool italic = false;
    int fontSizePt = 0;
};

class WordEditor {
public:
    // The selected words must be consecutive in reading order. Unchanged text anchors
    // retain its original box; changed spans share or divide the source geometry.
    static std::expected<OcrPage, QString> apply(const OcrPage &page,
        const QVector<int> &selection, const QVector<EditedWord> &replacement);
};

} // namespace yagf::core

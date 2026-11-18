#pragma once

#include "ocrdocument.h"

#include <expected>

namespace yagf::core {

struct EditedWord {
    QString text;
    bool bold = false;
    bool italic = false;
    int fontSizePt = 0;
    QString fontFamily;
};

class WordEditor {
public:
    static std::expected<OcrPage, QString> acceptSpecialist(const OcrPage &page, int anchor,
                                                           const QString &reading);
    static std::expected<OcrPage, QString> acceptPassage(const OcrPage &page, const QVector<int> &span,
                                                        const QString &reading);
    // The selected words must be consecutive in reading order. Unchanged text anchors
    // retain its original box; changed spans share or divide the source geometry.
    static std::expected<OcrPage, QString> apply(const OcrPage &page,
        const QVector<int> &selection, const QVector<EditedWord> &replacement);
};

} // namespace yagf::core

#pragma once

#include "ocrdocument.h"

#include <QImage>
#include <stop_token>

namespace yagf::processing {

struct MathSymbol {
    QRect bounds;
    bool largeOperator = false;
    QString latex; // Available only when the image distinguishes this exact glyph.
    double error = 1;
    double margin = 0;
    qint64 elapsedMs = 0;
    QString bankHash;
};

// Locate isolated mathematical glyphs, keeping native word boxes as evidence.
QVector<MathSymbol> findMathSymbols(const QImage &gray, QRect area,
    const QVector<core::OcrWord> &words, bool mathOnly, std::stop_token stop);

} // namespace yagf::processing

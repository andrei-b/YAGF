#pragma once

#include "ocrdocument.h"

#include <QImage>
#include <stop_token>

namespace yagf::processing {

// Refine the OCR crop rectangles in displayed-page coordinates.
QVector<core::ImageRegion> growFormulaBlocks(const QImage &image,
    const QVector<core::ImageRegion> &blocks, std::stop_token stop = {});

// Grow one recognition crop within a caller-supplied search window. Protected
// boxes are prose or other content that the formula engine must not consume.
QRect growSymbolCrop(const QImage &gray, QRect seed, QRect window,
    const QVector<QRect> &protectedBoxes, bool largeOperator, std::stop_token stop);

} // namespace yagf::processing

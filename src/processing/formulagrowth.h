#pragma once

#include "ocrdocument.h"

#include <QImage>
#include <stop_token>

namespace yagf::processing {

// Refine the OCR crop rectangles in displayed-page coordinates.
QVector<core::ImageRegion> growFormulaBlocks(const QImage &image,
    const QVector<core::ImageRegion> &blocks, std::stop_token stop = {});

// Bounding box of dark ink in a grayscale image, clipped to the image frame.
QRect inkBox(const QImage &gray, QRect area);

// A negative formula index identifies prose. The index otherwise refers to the
// input hint, so a caller can retain the model's classification and score.
struct InlinePart {
    QRect bounds;
    int formula = -1;
    bool containsUnsplitFormula = false;
    QString content() const {
        if (formula >= 0) return QStringLiteral("math");
        return containsUnsplitFormula ? QStringLiteral("mixed") : QStringLiteral("text");
    }
};
QVector<InlinePart> partitionInlineFormulas(const QImage &gray, QRect parent,
                                          const QVector<QRect> &hints);
QVector<QRect> findInlineFormulaHints(const QImage &gray, QRect parent, std::stop_token stop = {});
QVector<core::ImageRegion> refineFormulaRegions(const QImage &image,
    const QVector<core::ImageRegion> &regions, const QVector<core::OcrWord> &words = {}, std::stop_token stop = {});

// Grow one recognition crop within a caller-supplied search window. Protected
// boxes are prose or other content that the formula engine must not consume.
QRect growSymbolCrop(const QImage &gray, QRect seed, QRect window,
    const QVector<QRect> &protectedBoxes, bool largeOperator, std::stop_token stop);

} // namespace yagf::processing

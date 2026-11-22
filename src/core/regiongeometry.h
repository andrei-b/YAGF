#pragma once

#include <QRectF>
#include <QImage>
#include <QVector>

namespace yagf::core {

// Shared whitespace can belong to adjacent OCR crops. Without page pixels,
// use the conservative rectangle-only check.
class RegionInk {
public:
    RegionInk() = default;
    explicit RegionInk(const QImage &page);
    bool conflicts(const QRect &a, const QRect &b) const;
    const QImage &grayscale() const { return gray; }
private:
    QImage gray;
    int inkThreshold = 245;
};

// Follow a move or resize only as far as the first obstacle, including large pointer jumps.
QRect constrainRegion(const QRectF &start, const QRectF &desired, const QVector<QRectF> &obstacles,
                      const RegionInk &ink = {});

} // namespace yagf::core

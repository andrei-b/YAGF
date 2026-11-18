#pragma once

#include <QRectF>
#include <QVector>

namespace yagf::core {

// Follow a move or resize only as far as the first obstacle, including large pointer jumps.
QRect constrainRegion(const QRectF &start, const QRectF &desired, const QVector<QRectF> &obstacles);

} // namespace yagf::core

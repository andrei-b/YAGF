#pragma once

#include <QColor>
#include <QImage>
#include <QRect>

namespace yagf::processing::detail {

QColor paperColor(const QImage &image, const QRect &box);

} // namespace yagf::processing::detail

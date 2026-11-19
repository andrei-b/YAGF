#pragma once
#include <QImage>
#include <QPolygonF>
#include <QRect>
#include <expected>

namespace yagf::processing::restoration {
enum class BlurProfile { Motion, SoftFocus };
using Result = std::expected<QImage, QString>;
Result sharpnessMap(const QImage &page);
Result mergeCaptures(const QImage &reference, const QImage &second);
Result straighten(const QImage &page, QVector<QPolygonF> guides);
// A smaller output rectangle computes exactly the same preview tiles as the full operation.
Result deblur(const QImage &page, QRect region, double length, double degrees, double strength,
              QRect output = {}, BlurProfile profile = BlurProfile::Motion);
}

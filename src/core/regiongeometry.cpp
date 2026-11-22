#include "regiongeometry.h"
#include "ocrdocument.h"

#include <QPainter>

#include <algorithm>
#include <array>
#include <cmath>

namespace yagf::core {

QRegion ImageRegion::shape() const
{
    if (parts.isEmpty()) return bounds;
    QRegion result;
    for (const auto &part : parts) result |= part.translated(bounds.topLeft());
    return result;
}

void ImageRegion::setShape(const QRegion &pixels)
{
    bounds = pixels.boundingRect();
    parts.clear();
    if (pixels.rectCount() > 1)
        for (QRect piece : pixels) parts.append(piece.translated(-bounds.topLeft()));
}

void ImageRegion::resizeBounds(QRect target)
{
    if (parts.isEmpty() || target.size() == bounds.size()) { bounds = target; return; }
    const auto scale = QTransform::fromScale(double(target.width())/bounds.width(), double(target.height())/bounds.height());
    QRegion resized;
    for (const auto &part : parts) resized |= scale.mapRect(part);
    setShape(resized.translated(target.topLeft()));
}

QImage ImageRegion::crop(const QImage &page) const
{
    if (page.isNull() || bounds.isEmpty()) return {};
    QImage cropped = page.copy(bounds);
    if (!parts.isEmpty()) {
        cropped = cropped.convertToFormat(QImage::Format_RGB32);
        QPainter paint(&cropped);
        paint.setClipRegion(QRegion(cropped.rect()).subtracted(shape().translated(-bounds.topLeft())));
        paint.fillRect(cropped.rect(), Qt::white);
    }
    return cropped;
}

RegionInk::RegionInk(const QImage &page)
{
    if (page.hasAlphaChannel()) {
        gray = QImage(page.size(), QImage::Format_Grayscale8);
        gray.fill(Qt::white);
        QPainter painter(&gray);
        painter.drawImage(0, 0, page);
    } else {
        gray = page.convertToFormat(QImage::Format_Grayscale8);
    }
    if (gray.isNull()) return;
    // Estimate paper brightness from a small grid so aged paper can still
    // count as whitespace. Keep faint strokes as obstacles.
    std::array<int, 256> histogram{};
    int samples = 0;
    for (int y = 0; y < gray.height(); y += std::max(1, gray.height() / 100)) {
        const auto *row = gray.constScanLine(y);
        for (int x = 0; x < gray.width(); x += std::max(1, gray.width() / 100)) {
            ++histogram[row[x]];
            ++samples;
        }
    }
    int bright = 0;
    for (int i = 255; i >= 0; --i) {
        bright += histogram[i];
        if (bright * 10 >= samples) {
            inkThreshold = std::clamp(i - 8, 160, 245);
            break;
        }
    }
}

bool RegionInk::conflicts(const QRect &a, const QRect &b) const
{
    const QRect shared = a.intersected(b);
    if (shared.isEmpty()) return false;
    const auto area = [](QRect r) { return qint64(r.width()) * r.height(); };
    if (!gray.rect().contains(shared) || 5 * area(shared) > std::min(area(a), area(b))) return true;
    for (int y = shared.top(); y <= shared.bottom(); ++y) {
        const auto *begin = gray.constScanLine(y) + shared.left();
        if (std::any_of(begin, begin + shared.width(), [&](uchar p) { return p < inkThreshold; })) return true;
    }
    return false;
}

bool RegionInk::shapeConflicts(const QRegion &a, const QRegion &b) const
{
    if (a.rectCount() == 1 && b.rectCount() == 1) return conflicts(a.boundingRect(),b.boundingRect());
    const auto shared = a.intersected(b);
    if (shared.isEmpty()) return false;
    const auto area = [](const QRegion &region) {
        qint64 count = 0;
        for (QRect r : region) count += qint64(r.width()) * r.height();
        return count;
    };
    if (!gray.rect().contains(shared.boundingRect()) || 5*area(shared) > std::min(area(a),area(b))) return true;
    for (QRect r : shared) for (int y=r.top(); y<=r.bottom(); ++y) {
        const auto *row = gray.constScanLine(y);
        if (std::any_of(row+r.left(),row+r.right()+1,[&](uchar value){return value < inkThreshold;})) return true;
    }
    return false;
}

QRect constrainRegion(const QRectF &start, const QRectF &desired, const QVector<QRectF> &obstacles, const RegionInk &ink)
{
    const auto at = [&](double t) {
        const auto edge = [t](double a, double b) { return qRound(a + (b - a) * t); };
        const int left = edge(start.left(), desired.left()), top = edge(start.top(), desired.top());
        return QRect(left, top, edge(start.right(), desired.right()) - left,
                     edge(start.bottom(), desired.bottom()) - top);
    };
    double stop = 1;
    for (const auto &box : obstacles) {
        // Older projects may already overlap. Let the user move those blocks apart.
        if (start.intersects(box) && ink.conflicts(start.toRect(), box.toRect())) continue;
        double enter = 0, leave = 1;
        bool possible = true;
        const auto positive = [&](double initial, double final) {
            const double delta = final - initial;
            if (std::abs(delta) < 1e-10) {
                if (initial <= 0) possible = false;
            } else if (delta > 0) enter = std::max(enter, -initial / delta);
            else leave = std::min(leave, -initial / delta);
        };
        positive(start.right() - box.left(), desired.right() - box.left());
        positive(box.right() - start.left(), box.right() - desired.left());
        positive(start.bottom() - box.top(), desired.bottom() - box.top());
        positive(box.bottom() - start.top(), box.bottom() - desired.top());
        if (!possible || enter >= leave || enter >= stop || leave <= 0) continue;
        if (ink.grayscale().isNull()) {
            stop = std::min(stop, enter);
            continue;
        }
        // Walk only the part of the gesture inside this obstacle. Checking the
        // endpoint alone lets a fast drag jump over a glyph into a blank margin.
        const double end = std::min(stop, leave);
        const double distance = std::max({std::abs(desired.left() - start.left()), std::abs(desired.top() - start.top()),
                                          std::abs(desired.right() - start.right()), std::abs(desired.bottom() - start.bottom())});
        const int steps = std::max(1, int(std::ceil(distance * (end - enter))));
        double previous = enter;
        for (int i = 1; i <= steps; ++i) {
            const double t = enter + (end - enter) * i / steps;
            if (ink.conflicts(at(t), box.toRect())) { stop = previous; break; }
            previous = t;
        }
    }
    return at(stop);
}

} // namespace yagf::core

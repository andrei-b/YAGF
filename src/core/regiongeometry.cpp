#include "regiongeometry.h"

#include <QPainter>

#include <algorithm>
#include <array>
#include <cmath>

namespace yagf::core {

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

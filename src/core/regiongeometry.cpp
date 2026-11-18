#include "regiongeometry.h"

#include <algorithm>
#include <cmath>

namespace yagf::core {

QRect constrainRegion(const QRectF &start, const QRectF &desired, const QVector<QRectF> &obstacles)
{
    double stop = 1;
    for (const auto &box : obstacles) {
        // Older projects may already overlap. Let the user move those blocks apart.
        if (start.intersects(box)) continue;
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
        if (possible && enter < leave && enter < 1 && leave > 0) stop = std::min(stop, std::max(0.0, enter));
    }
    const auto interpolate = [stop](double from, double to) { return qRound(from + (to - from) * stop); };
    const int left = interpolate(start.left(), desired.left()), top = interpolate(start.top(), desired.top());
    return {left, top, interpolate(start.right(), desired.right()) - left,
                       interpolate(start.bottom(), desired.bottom()) - top};
}

} // namespace yagf::core

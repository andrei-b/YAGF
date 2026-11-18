#include "pagebackground.h"

#include <QVector>
#include <algorithm>

namespace yagf::processing::detail {

QColor paperColor(const QImage &image, const QRect &box)
{
    // Sample the surrounding paper so replacing text also works on tinted pages.
    const QRect edge = box.adjusted(-2, -2, 2, 2).intersected(image.rect());
    QVector<int> red, green, blue;
    const auto sample = [&](int x, int y) {
        const QColor color = image.pixelColor(x, y);
        red.append(color.red()); green.append(color.green()); blue.append(color.blue());
    };
    for (int x = edge.left(); x <= edge.right(); x += std::max(1, edge.width() / 64)) {
        sample(x, edge.top()); sample(x, edge.bottom());
    }
    for (int y = edge.top(); y <= edge.bottom(); y += std::max(1, edge.height() / 64)) {
        sample(edge.left(), y); sample(edge.right(), y);
    }
    if (red.isEmpty()) return Qt::white;
    std::ranges::sort(red); std::ranges::sort(green); std::ranges::sort(blue);
    const qsizetype index = red.size() * 3 / 4;
    return QColor(red[index], green[index], blue[index]);
}

} // namespace yagf::processing::detail

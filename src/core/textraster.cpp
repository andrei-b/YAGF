#include "textraster.h"
#include <QFontMetricsF>
#include <QPainter>
#include <QTextLayout>
#include <algorithm>
#include <cmath>
#include <limits>

namespace yagf::core::typeset {
Ink ink(const QImage &input, bool firstLine)
{
    if (input.isNull()) return {};
    QImage gray = input.convertToFormat(QImage::Format_Grayscale8);
    // Ignore light paper/antialiasing noise when locating the actual letter edges.
    int left = gray.width(), right = -1, top = gray.height(), bottom = -1;
    int emptyRows = 0;
    for (int y = 0; y < gray.height(); ++y) {
        const auto *row = gray.constScanLine(y);
        bool occupied = false;
        for (int x = 0; x < gray.width(); ++x) {
            if (row[x] >= 160) continue;
            left = std::min(left, x);
            right = std::max(right, x);
            top = std::min(top, y);
            bottom = y;
            occupied = true;
        }
        if (occupied) emptyRows = 0;
        else if (bottom >= 0 && ++emptyRows >= 3 && firstLine) break;
    }
    if (right < left || bottom < top) return {};
    const QRect bounds(QPoint(left, top), QPoint(right, bottom));
    return {gray.copy(bounds).scaled(96, 32, Qt::IgnoreAspectRatio, Qt::SmoothTransformation), bounds.size()};
}


Raster raster(const QString &text, QFont font, const QSize &sourceSize, bool passage)
{
    font.setPixelSize(64);
    const QRectF reference = QFontMetricsF(font).tightBoundingRect(text.left(120));
    if (reference.height() < 1) return {};
    const int pixels = std::clamp(qRound(64.0 * sourceSize.height() / reference.height()), 6, 256);
    font.setPixelSize(pixels);
    QString lineText = text;
    if (passage && QFontMetricsF(font).horizontalAdvance(text) > sourceSize.width() * 1.08) {
        QTextLayout layout(text, font);
        QTextOption option;
        option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        layout.setTextOption(option);
        layout.beginLayout();
        auto line = layout.createLine();
        if (!line.isValid()) { layout.endLayout(); return {}; }
        line.setLineWidth(sourceSize.width() + 2);
        lineText = text.left(line.textLength()).trimmed();
        layout.endLayout();
    }
    const QFontMetricsF metrics(font);
    const QRectF bounds = metrics.tightBoundingRect(lineText);
    const int width = qCeil(std::max(bounds.width(), metrics.horizontalAdvance(lineText))) + 12;
    const int height = qCeil(metrics.height()) + 12;
    if (width < 1 || width > 8192 || height > 1024) return {};
    QImage result(width, height, QImage::Format_RGB32);
    result.fill(Qt::white);
    QPainter painter(&result);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.setFont(font);
    painter.setPen(Qt::black);
    painter.drawText(QPointF(6 - std::min<qreal>(0, bounds.left()), 6 + metrics.ascent()), lineText);
    painter.end();
    return {ink(result, false), pixels};
}

double distance(const Ink &source, const Ink &candidate)
{
    if (candidate.mask.isNull()) return std::numeric_limits<double>::infinity();
    double difference = 0;
    for (int y = 0; y < source.mask.height(); ++y) {
        const auto *a = source.mask.constScanLine(y);
        const auto *b = candidate.mask.constScanLine(y);
        for (int x = 0; x < source.mask.width(); ++x) difference += std::abs(int(a[x]) - int(b[x]));
    }
    const double ratio = (double(candidate.size.width()) / candidate.size.height()) /
                         (double(source.size.width()) / source.size.height());
    return difference / (255 * 96 * 32) + 0.15 * std::abs(std::log(ratio));
}
}

#include "symbolshape.h"

#include <QPainter>
#include <algorithm>

namespace yagf::processing {

QByteArray symbolShape(const QImage &page, const QRect &bounds)
{
    if (bounds.isEmpty() || !page.rect().contains(bounds)) return {};
    // Keep aspect ratio: a narrow stem must not become a wide bar. OCR boxes
    // already locate the ink. A small fixed mask tolerates changes in print size.
    QImage crop = page.copy(bounds);
    if (crop.hasAlphaChannel()) {
        // PNG scans can have transparent paper.
        QImage paper(crop.size(), QImage::Format_RGB32);
        paper.fill(Qt::white);
        QPainter painter(&paper);
        painter.drawImage(0, 0, crop);
        painter.end();
        crop = std::move(paper);
    }
    const QImage sample = crop.scaled(16, 16, Qt::KeepAspectRatio, Qt::SmoothTransformation)
        .convertToFormat(QImage::Format_Grayscale8);
    if (sample.isNull()) return {};
    int low = 255, high = 0;
    for (int y = 0; y < sample.height(); ++y) {
        const auto row = sample.constScanLine(y);
        const auto [min, max] = std::minmax_element(row, row + sample.width());
        low = std::min(low, int(*min));
        high = std::max(high, int(*max));
    }
    if (high - low < 32) return {};
    const int threshold = (high + low) / 2;
    const int left = (16 - sample.width()) / 2, top = (16 - sample.height()) / 2;
    QByteArray mask(32, 0);
    int ink = 0;
    for (int y = 0; y < sample.height(); ++y) {
        const auto row = sample.constScanLine(y);
        for (int x = 0; x < sample.width(); ++x) {
            if (row[x] > threshold) continue;
            const int bit = (top + y) * 16 + left + x;
            mask[bit / 8] |= char(1u << (bit % 8));
            ++ink;
        }
    }
    return ink >= 4 ? mask : QByteArray{};
}

} // namespace yagf::processing

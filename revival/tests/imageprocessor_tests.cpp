#include "imageprocessor.h"

#include <QPainter>
#include <QTransform>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>

using yagf::core::PreprocessSettings;
using yagf::processing::ImageProcessor;

int main()
{
    QImage clean(900, 650, QImage::Format_ARGB32);
    clean.fill(Qt::white);
    {
        QPainter painter(&clean);
        painter.setPen(Qt::NoPen);
        painter.setBrush(Qt::black);
        for (int line = 0; line < 7; ++line)
            for (int word = 0; word < 6; ++word)
                painter.drawRect(90 + word * 115, 80 + line * 75, 55 + (word % 3) * 11, 10);
    }
    const QImage skewed = clean.transformed(QTransform().rotate(4.0), Qt::SmoothTransformation);
    const auto assessment = ImageProcessor::assess(skewed);
    assert(assessment.has_value());
    assert(std::abs(assessment->skewDegrees - 4.0) < 1.5);
    const auto corrected = ImageProcessor::apply(skewed, {.deskewDegrees = assessment->skewDegrees});
    assert(corrected.has_value());
    const auto after = ImageProcessor::assess(*corrected);
    assert(after.has_value());
    assert(std::abs(after->skewDegrees) < 1.0);

    QImage faded(25, 25, QImage::Format_ARGB32);
    faded.fill(qRgb(200, 200, 200));
    for (int y = 4; y < 20; ++y)
        for (int x = 4; x < 20; ++x) faded.setPixel(x, y, qRgb(115, 115, 115));
    faded.setPixel(2, 2, Qt::black);
    const auto improved = ImageProcessor::apply(faded, {.autoContrast = true, .denoise = true});
    assert(improved.has_value());
    assert(qGray(improved->pixel(22, 22)) > 240);
    assert(qGray(improved->pixel(10, 10)) < 20);
    assert(qGray(improved->pixel(2, 2)) > 150);
    QImage faint(25, 25, QImage::Format_ARGB32);
    faint.fill(qRgb(205, 205, 205));
    for (int y = 4; y < 20; ++y)
        for (int x = 4; x < 20; ++x) faint.setPixel(x, y, qRgb(198, 198, 198));
    const auto restored = ImageProcessor::apply(faint, {.autoContrast = true});
    assert(restored.has_value());
    assert(qGray(restored->pixel(10, 10)) < 20);
    assert(qGray(restored->pixel(22, 22)) > 240);
    assert(!ImageProcessor::assess(QImage()).has_value());
    assert(!ImageProcessor::apply(clean, {.deskewDegrees = 25.0}).has_value());
}

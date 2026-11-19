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
    // Strong shadows make a global Otsu threshold classify much of the paper
    // as ink. Local binarization must still recover the text's actual tilt.
    QImage uneven(1200,900,QImage::Format_RGB32);
    for(int y=0;y<uneven.height();++y) for(int x=0;x<uneven.width();++x) {
        const int paper=75+170*x/uneven.width();
        uneven.setPixel(x,y,qRgb(paper,paper,paper));
    }
    {
        QPainter painter(&uneven);
        for(int row=0;row<16;++row)for(int column=0;column<30;++column) {
            const int x=80+column*34, y=80+row*44;
            const int ink=qMax(0,75+170*x/uneven.width()-55);
            painter.fillRect(x,y,18,10,QColor(ink,ink,ink));
        }
    }
    uneven.setDotsPerMeterX(11811);uneven.setDotsPerMeterY(11811);
    const auto tilted=uneven.transformed(QTransform().rotate(5.0),Qt::SmoothTransformation);
    const auto unevenAssessment=ImageProcessor::assess(tilted);
    assert(unevenAssessment && std::abs(unevenAssessment->skewDegrees-5.0)<0.6);
    const auto binary=ImageProcessor::apply(uneven,{.adaptiveBinarization=true});
    assert(binary && qGray(binary->pixel(10,20))==255 && qGray(binary->pixel(1000,20))==255);
    assert(qGray(binary->pixel(82,82))==0);
    assert(binary->dotsPerMeterY()==11811);
    // Confident words calibrate the entire page, including unread strokes
    // outside those boxes. Speckles on the paper should disappear.
    QImage speckled = uneven;
    for (int y = 5; y < 55; y += 7) {
        for (int x = 20; x < 1100; x += 9) {
            const int speck = qGray(uneven.pixel(x, y)) - 25;
            speckled.setPixel(x, y, qRgb(speck, speck, speck));
        }
    }
    const QVector<QRect> boxes{{76,76,28,18}, {110,76,28,18}, {144,76,28,18}};
    const int threshold = ImageProcessor::estimateInkThreshold(speckled, boxes);
    assert(threshold > 0);
    const auto guided = ImageProcessor::apply(speckled, {.adaptiveBinarization=true, .inkThreshold=threshold});
    const auto legacy = ImageProcessor::apply(speckled, {.adaptiveBinarization=true});
    assert(guided && legacy);
    assert(qGray(legacy->pixel(20,5)) == 0 && qGray(guided->pixel(20,5)) == 255);
    assert(qGray(guided->pixel(423,390)) == 0);
    assert(ImageProcessor::estimateInkThreshold(speckled, {}) == 0);
    const auto rotate=ImageProcessor::apply(uneven,{.quarterTurns=1,.adaptiveBinarization=true});
    assert(rotate && rotate->size()==QSize(900,1200));
    assert(!ImageProcessor::apply(uneven,{.quarterTurns=4}));

}

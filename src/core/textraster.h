#pragma once
#include <QFont>
#include <QImage>

namespace yagf::core::typeset {
struct Ink {
    QImage mask;
    QSize size;
};
struct Raster {
    Ink ink;
    int pixelSize = 0;
};
Ink ink(const QImage &input, bool firstLine);
Raster raster(const QString &text, QFont font, const QSize &sourceSize, bool passage);
double distance(const Ink &source, const Ink &candidate);
}

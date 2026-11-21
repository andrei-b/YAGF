#pragma once
#include "ocrdocument.h"
#include <QImage>
#include <QPainter>

namespace yagf::tests {
inline QImage mathGravityImage() {
    QImage image(220,180,QImage::Format_RGB32);image.fill(Qt::white);
    QPainter painter(&image);painter.setPen(Qt::NoPen);painter.setBrush(Qt::black);
    for(const auto &box:QVector<QRect>{{90,40,9,20},{108,40,9,20},{126,40,9,20},
        {114,76,6,8},{110,89,5,34},{130,100,10,22},{149,100,10,22}})painter.drawRect(box);
    return image;
}
inline QVector<core::ImageRegion> mathGravityRegions() {
    return {{"text",{20,20,175,65},"rus",false,"text",true,"text",.95},
            {"formula",{70,85,100,55},"auto",false,"math",true,"display_formula",.9}};
}
}

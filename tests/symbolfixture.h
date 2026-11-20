#pragma once
#include "symbolaugmentation.h"
#include <QPainter>
#include <QPainterPath>

inline QImage symbolFixture(bool psi = true, int stroke = 3)
{
    QImage image(40,48,QImage::Format_RGB32); image.fill(Qt::white);
    QPainter p(&image); p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(Qt::black,stroke,Qt::SolidLine,Qt::RoundCap));
    p.drawLine(20,5,20,43);
    if (psi) { QPainterPath path; path.moveTo(7,9); path.lineTo(7,22); path.cubicTo(7,36,33,36,33,22); path.lineTo(33,9); p.drawPath(path); }
    else p.drawEllipse(QRectF(6,9,28,27));
    return image;
}
inline yagf::core::SymbolRule symbolRuleFixture()
{
    using namespace yagf;
    core::SymbolRule rule; rule.id="psi-rule"; rule.label=QString::fromUtf8("ψ"); rule.language="rus";
    rule.seedPage=1; rule.seedDpi=150; rule.seedBounds={10,10,40,48};
    rule.seedPng=processing::symbolCropPng(symbolFixture());
    for (int i=0;i<3;++i) {
        const auto image=symbolFixture(i<2,i==1?4:3);
        core::SymbolExample e; e.id=QString::number(i); e.page=1; e.bounds={10+i*55,10,40,48};
        e.png=processing::symbolCropPng(image); e.mask=processing::symbolFeature(image); e.reading=QString::fromUtf8("ф");
        e.decision=i<2?"positive":"negative"; rule.examples.append(e);
    }
    return rule;
}

inline QImage smileyFixture(bool happy=true,int stroke=2)
{
    QImage image(40,40,QImage::Format_RGB32); image.fill(Qt::white);
    QPainter p(&image); p.setRenderHint(QPainter::Antialiasing); p.setPen(QPen(Qt::black,stroke,Qt::SolidLine,Qt::RoundCap));
    p.drawEllipse(QRectF(3,3,34,34)); p.drawPoint(13,14); p.drawPoint(27,14);
    QPainterPath mouth; mouth.moveTo(11,happy?23:30); mouth.quadTo(20,happy?37:16,29,happy?23:30); p.drawPath(mouth);
    return image;
}

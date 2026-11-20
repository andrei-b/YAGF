#pragma once
#include <QIcon>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

namespace yagf::ui {
inline QIcon aiPageAreasIcon()
{
    QIcon icon;
    for (int size : {24, 32, 48, 64}) {
        QPixmap pixmap(size,size); pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap); painter.setRenderHint(QPainter::Antialiasing);
        painter.scale(size/32.,size/32.);
        painter.setPen(QPen(QColor("#216ec1"),5,Qt::SolidLine,Qt::RoundCap));
        painter.drawLine(QPointF(6,26),QPointF(23,9));
        painter.setPen(QPen(QColor("#c9dff8"),2,Qt::SolidLine,Qt::RoundCap));
        painter.drawLine(QPointF(6,26),QPointF(19,13));
        painter.setPen(QPen(QColor("#c18a3a"),4,Qt::SolidLine,Qt::RoundCap));
        painter.drawLine(QPointF(22,10),QPointF(25,7));
        const auto sparkle=[&](double x,double y,double radius,QColor color) {
            QPainterPath path; path.moveTo(x,y-radius); path.quadTo(x,y,x+radius,y);
            path.quadTo(x,y,x,y+radius); path.quadTo(x,y,x-radius,y); path.quadTo(x,y,x,y-radius);
            painter.fillPath(path,color);
        };
        sparkle(17,5,4,QColor("#216ec1")); sparkle(28,17,3.5,QColor("#c18a3a"));
        sparkle(8,12,2.5,QColor("#216ec1"));
        painter.end(); icon.addPixmap(pixmap);
    }
    return icon;
}
}

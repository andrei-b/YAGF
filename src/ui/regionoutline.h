#pragma once

#include "ocrdocument.h"
#include <QGraphicsRectItem>
#include <QPainter>

namespace yagf::ui {

// Keep the rectangular resize handles but paint and hit-test the actual block.
class RegionOutline : public QGraphicsRectItem {
public:
    explicit RegionOutline(const core::ImageRegion &block)
        : QGraphicsRectItem(block.bounds), original(block) {}

    core::ImageRegion blockAt(QRect bounds) const {
        auto result = original;
        result.resizeBounds(bounds);
        return result;
    }
    QPainterPath shape() const override {
        if (original.parts.isEmpty()) return QGraphicsRectItem::shape();
        QPainterPath path;
        path.addRegion(blockAt(rect().toRect()).shape());
        return path.simplified();
    }
    void paint(QPainter *p,const QStyleOptionGraphicsItem *option,QWidget *widget) override {
        if (original.parts.isEmpty()) { QGraphicsRectItem::paint(p,option,widget); return; }
        p->setPen(pen()); p->setBrush(brush()); p->drawPath(shape());
    }
private:
    core::ImageRegion original;
};
}

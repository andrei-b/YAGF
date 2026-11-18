#include "blockminiaturelist.h"

#include <QContextMenuEvent>

namespace yagf::ui {

BlockMiniatureList::BlockMiniatureList(QWidget *parent) : QListWidget(parent)
{
    setViewMode(QListView::IconMode);
    setFlow(QListView::TopToBottom);
    setWrapping(false);
    setMovement(QListView::Static);
    setResizeMode(QListView::Adjust);
    setGridSize(QSize(168, 142));
    setIconSize(QSize(146, 92));
}

void BlockMiniatureList::contextMenuEvent(QContextMenuEvent *event)
{
    QListWidgetItem *item = nullptr;
    QPoint position = event->globalPos();
    if (event->reason() == QContextMenuEvent::Keyboard) {
        item = currentItem();
        if (item) {
            scrollToItem(item);
            position = viewport()->mapToGlobal(visualItemRect(item).center());
        }
    } else {
        item = itemAt(event->pos());
        // Icon-mode items need not fill the width of their thumbnail tile.
        // Treat its surrounding horizontal space as the same block.
        if (!item && viewport()->rect().contains(event->pos())) {
            for (int index = 0; index < count(); ++index) {
                const QRect bounds = visualItemRect(this->item(index));
                const QRect tile(0, bounds.top(), viewport()->width(), gridSize().height());
                if (tile.contains(event->pos())) { item = this->item(index); break; }
            }
        }
    }
    if (!item) { event->ignore(); return; }
    const int index = row(item);
    event->accept();
    emit blockContextMenuRequested(index, position);
}

} // namespace yagf::ui

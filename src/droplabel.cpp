#include "droplabel.h"
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QUrl>
#include <QStandardItemModel>
#include <QtDebug>
#include <QListWidget>
#include <QList>

DropLabel::DropLabel(QWidget *parent, Qt::WindowFlags f) : QLabel(parent, f)
{
    setAcceptDrops(true);
}

void DropLabel::dragEnterEvent(QDragEnterEvent *event)
{
    //event->setDropAction(Qt::IgnoreAction);
    const QMimeData * md = event->mimeData();
    QStringList sl = md->formats();
    if (!md->formats().contains("text/uri-list")) {
       setCursor(Qt::ForbiddenCursor);
    }
    else {
    event->setDropAction(Qt::MoveAction);
    event->accept();
    //QCursor cur(lw->selectedItems().at(0)->icon().pixmap(96, 128));
    //setCursor(cur);
    }

}

void DropLabel::dragLeaveEvent(QDragLeaveEvent *event)
{
    setCursor(Qt::ArrowCursor);
    event->accept();
}

void DropLabel::dropEvent(QDropEvent *event)
{
  //  QList<QListWidgetItem *> lwi = lw->selectedItems();
    if (event->mimeData()->formats().contains("text/uri-list")) {
        setCursor(Qt::ArrowCursor);
        event->setDropAction(Qt::MoveAction);
        event->accept();

    }
    //    foreach(QListWidgetItem * iw, lwi)
    //        lw->model()->removeRow(lw->row(iw));
    //}
}

void DropLabel::setListWidget(QListWidget *w)
{
    lw = 0;
}

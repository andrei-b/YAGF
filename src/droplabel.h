#ifndef DROPLABEL_H
#define DROPLABEL_H
#include <QLabel>

class QListWidget;

class DropLabel : public QLabel
{
public:
    DropLabel(QWidget * parent = 0, Qt::WindowFlags f = 0);
    void setListWidget(QListWidget * w);
protected:
    void dragEnterEvent(QDragEnterEvent *event);
    void dragLeaveEvent(QDragLeaveEvent *event);
    void dropEvent(QDropEvent *event);
private:
   QListWidget * lw;
};

#endif // DROPLABEL_H

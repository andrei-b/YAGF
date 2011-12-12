#include "src/qsnippet.h"

QSnippet::QSnippet(QListWidget *parent) :
    QListWidgetItem(parent)
{
    rotation = 0;
    scale = 1;

}

void QSnippet::addFile(const QString &name, const QPixmap *pixmap)
{
    if (!pixmap)
        setIcon(QPixmap(name));
    else
        setIcon(*pixmap);
    this->name = name;
    setToolTip(name);
}

void QSnippet::setRotation(int rot)
{
    rotation = rot;
}

int QSnippet::getRotation()
{
    return rotation;
}

void QSnippet::setScale(double sc)
{
    scale = sc;
}

double QSnippet::getScale()
{
    return scale;
}

QString QSnippet::getName()
{
    return name;
}

BolockList * QSnippet::blocks()
{
    return &blockList;
}

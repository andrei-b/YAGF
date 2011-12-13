#include "src/qsnippet.h"

QSnippet::QSnippet(QListWidget *parent) :
    QListWidgetItem(parent)
{
    rotation = 0;
    scale = 1;

}

bool QSnippet::addFile(const QString &name, const QPixmap *pixmap)
{
    if (!pixmap) {
        QPixmap pm(name);
        if (pm.isNull())
            return false;
        setIcon(pm);
    }
    else
        setIcon(*pixmap);
    this->name = name;
    setToolTip(name);
    return true;
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

#ifndef QSNIPPET_H
#define QSNIPPET_H

#include <QListWidget>
#include <QListWidgetItem>
#include <QList>
#include <QRect>
#include <QPixmap>

typedef QList<QRect> BolockList;

class QSnippet : public QListWidgetItem
{
public:
    explicit QSnippet(QListWidget *parent = 0);
    void addFile(const QString &name, const QPixmap * pixmap = NULL);
    void setRotation(int rot);
    int getRotation();
    void setScale(double sc);
    double getScale();
    QString getName();
    BolockList * blocks();
signals:

public slots:
private:
    BolockList blockList;
    int rotation;
    double scale;
    QString name;
};

#endif // QSNIPPET_H

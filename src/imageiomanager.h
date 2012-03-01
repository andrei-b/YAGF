#ifndef IMAGEIOMANAGER_H
#define IMAGEIOMANAGER_H

#include <QObject>
#include <QImageReader>
#include <QPixmap>

class ImageIOManager : public QObject
{
    Q_OBJECT
public:
    explicit ImageIOManager(QObject *parent = 0);
    ~ImageIOManager();
    bool open(const QString &fileName);
    QPixmap & pixmap(int sf = 2);
    void clearFragment();
signals:

public slots:

private:
    void makeSmall();
    QString file;
    QImageReader ir;
    QImage *imgb2;
    QImage *imgb4;
    QImage *imgb8;
    QPixmap pmb2;
    QPixmap pmb4;
    QPixmap pmb8;
    QPixmap * fragment;
};

#endif // IMAGEIOMANAGER_H

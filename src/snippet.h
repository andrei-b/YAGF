#ifndef SNIPPET_H
#define SNIPPET_H

#include <QWidget>
#include <QString>
#include <QPixmap>
#include <QList>
#include <QRectF>

typedef QList<QRectF> BlockList;

namespace Ui {
    class Snippet;
}

class Snippet : public QWidget
{
    Q_OBJECT

public:
    explicit Snippet(QWidget *parent = 0);
    ~Snippet();
    QString getFullName() const;
    QString getName() const;
    void setImage(const QPixmap &pixmap, const QString &name);
    BlockList &getBlocks();
    int getRotation();
    void setRotation(const int rotation);
    void setScale(const float scale);
    float getScale();
signals:
    void selected(QString name);
private:
    Ui::Snippet *ui;
    QString name;
    BlockList blocks;
    int rotation;
    float scale;
};

#endif // SNIPPET_H

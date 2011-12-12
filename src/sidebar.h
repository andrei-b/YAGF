#ifndef SIDEBAR_H
#define SIDEBAR_H

#include <QListWidget>

class QSnippet;

class SideBar : public QListWidget
{
    Q_OBJECT
public:
    explicit SideBar(QWidget *parent = 0);
    void addFile(const QString &name, const QPixmap * pixmap = 0);
    QStringList getFileNames();
    void setRotation(int r, const QString &name = "");
    int getRotation(const QString &name = "");
    int getBlocksCount();
    void clearBlocks();
    void addBlock(const QRect &block, const QString &name = "");
    QRect getBlock(int index);
    void removeBlock(const QRect &block);
    void setScale(float s);
    float getScale(const QString &name = "");
    bool fileLoaded(const QString &name);
    void select(const QString &name);
    void selectFirstFile();
signals:
    void fileSelected(const QString &name);
private slots:
    void itemActive( QListWidgetItem * item );
protected:
    QStringList mimeTypes () const;
    Qt::DropActions supportedDropActions () const;
    bool dropMimeData(int index, const QMimeData *data, Qt::DropAction action);
    void startDrag(Qt::DropActions supportedActions);

    //bool dropMimeData(int index, const QMimeData *data, Qt::DropAction action);
    //QStringList mimeTypes();
    //void dragEnterEvent(QDragEnterEvent *event);
    //void dragLeaveEvent(QDragLeaveEvent *event);
    //void dropEvent(QDropEvent *event);
private:
    QSnippet * getItemByName(const QString &name);
    QSnippet * current;
};

#endif // SIDEBAR_H

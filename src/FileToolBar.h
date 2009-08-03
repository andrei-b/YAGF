#ifndef FILETOOLBAR_H
#define FILETOOLBAR_H

#include <QToolBar>
#include <QString>
#include <QMap>

typedef QMap<QString, QString> StringMap;

class QPixmap;

class FileToolBar : public QToolBar
{
Q_OBJECT
public:
    FileToolBar(QWidget * parent);
    void addFile(const QPixmap & pixmap, const QString & name);
signals:
    void fileSelected(const QString & name);
private:
    bool buttonsAdded;
    StringMap * filesMap;
private slots:
    void saveAll();
    void selected();
};


#endif // FILETOOLBAR_H

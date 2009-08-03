#ifndef FILETOOLBAR_H
#define FILETOOLBAR_H

#include <QToolBar>
#include <QString>
#include <QMap>

typedef QMap<QString, QString> StringMap;

class QPixmap;
class QPushButton;

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
    QPushButton * saveButton;
    QPushButton * clearButton;
private slots:
    void saveAll();
    void selected();
    void clearAll();
};


#endif // FILETOOLBAR_H

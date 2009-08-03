#include "FileToolBar.h"
#include <QSize>
#include <QAction>
#include <QIcon>
#include <QPixmap>
#include "utils.h"

FileToolBar::FileToolBar(QWidget * parent):QToolBar(trUtf8("Loaded Images"), parent)
{
    buttonsAdded = false;
    this->setIconSize(QSize(64, 96));
    this->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    filesMap = new StringMap();
}

void FileToolBar::addFile(const QPixmap & pixmap, const QString & name)
{
    if (!buttonsAdded) {
        addAction(QIcon(":/save_all.png"), trUtf8("Save Images"), this, "saveAll()");
        addAction(QIcon(":/clear.png"), trUtf8("Clear Gallery"), this, "saveAll()");
        buttonsAdded = true;
    }
    QPixmap pm = pixmap.scaledToHeight(96, Qt::FastTransformation);
    QString fn = extractFileName(name);
    filesMap->insert(fn, name);
    this->addAction(QIcon(pm), fn, this, "selected()");
}

void FileToolBar::selected()
{
}

void FileToolBar::saveAll()
{
}

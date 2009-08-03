#include "FileToolBar.h"
#include <QSize>
#include <QAction>
#include <QIcon>
#include <QPixmap>
#include <QPushButton>
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
    QAction * action;
    if (!buttonsAdded) {
        clearButton = new QPushButton(QIcon(":/clear.png"), trUtf8("Clear"), this);
        connect(clearButton, SIGNAL(clicked()), this, SLOT(clearAll()));
        action = insertWidget(0, clearButton);
        saveButton = new QPushButton(QIcon(":/save_all.png"), trUtf8("Save..."), this);
        connect(saveButton, SIGNAL(clicked()), this, SLOT(saveAll()));
        insertWidget(action, saveButton);
        buttonsAdded = true;
    }
    QPixmap pm = pixmap.scaledToHeight(96, Qt::FastTransformation);
    QString fn = extractFileName(name);
    if (!filesMap->contains(fn)) {
        filesMap->insert(fn, name);
        action = addAction(QIcon(pm), fn);
        connect(action, SIGNAL(triggered()), this, SLOT(selected()));
    }
}

void FileToolBar::selected()
{
    QString key = ((QAction *) sender())->text();
    QString path = filesMap->value(key);
    emit fileSelected(path);
}

void FileToolBar::saveAll()
{
}

void FileToolBar::clearAll()
{
    clear();
    buttonsAdded = false;
    filesMap->clear();
}

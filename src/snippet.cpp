#include "snippet.h"
#include "ui_snippet.h"
#include <QPalette>
#include <QFile>

Snippet::Snippet(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::Snippet)
{
    setAttribute(Qt::WA_DeleteOnClose);
    ui->setupUi(this);
    QPalette p;
    p.setColor(ui->label->backgroundRole(), QColor(100, 30, 40));
    ui->label->setPalette(p);
    connect(ui->label, SIGNAL(selected(QString)), this, SIGNAL(selected(QString)));
    connect(ui->pushButton, SIGNAL(clicked()), this, SLOT(close()));
    rotation = 0;
    scale = 1.0;
}

Snippet::~Snippet()
{
    delete ui;
}

void Snippet::setImage(const QPixmap &pixmap, const QString &name)
{
    QPixmap pm;
    if (pixmap.isNull()) {
        pm.load(name);
        pm = pm.scaledToWidth(100);
    } else
    pm = pixmap.scaledToWidth(100);
    ui->label->setPixmap(pm);
    ui->label->setName(name);
    this->name  = name;
    QFile f(name);
    setToolTip(f.fileName());
}

QString Snippet::getName() const
{
    return name;
}

QString Snippet::getFullName() const
{
    return name;
}

BlockList & Snippet::getBlocks()
{
    return blocks;
}

int Snippet::getRotation()
{
    return rotation;
}

void Snippet::setRotation(const int rotation)
{
    this->rotation = rotation;
}

void Snippet::setScale(const float scale)
{
    this->scale = scale;
}

float Snippet::getScale()
{
    return scale;
}

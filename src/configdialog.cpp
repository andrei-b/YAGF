#include "configdialog.h"
#include "ui_configdialog.h"
#include <QDir>
#include <QProcessEnvironment>

ConfigDialog::ConfigDialog(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::ConfigDialog)
{
    ui->setupUi(this);
}

ConfigDialog::~ConfigDialog()
{
    delete ui;
}

void ConfigDialog::setSelectedEngine(int value)
{
    if (!value)
        ui->radioButton->setChecked(true);
    else
        ui->radioButton_2->setChecked(true);
}

int ConfigDialog::selectedEngine()
{
    if (ui->radioButton->isChecked())
        return 0;
    return 1;
}

void ConfigDialog::setTessDataPath(const QString &value)
{
    if (value.isEmpty()) {

    }
}

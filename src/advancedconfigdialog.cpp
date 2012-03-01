#include "advancedconfigdialog.h"
#include "ui_advancedconfigdialog.h"

AdvancedConfigDialog::AdvancedConfigDialog(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::AdvancedConfigDialog)
{
    ui->setupUi(this);
}

AdvancedConfigDialog::~AdvancedConfigDialog()
{
    delete ui;
}

bool AdvancedConfigDialog::doCrop1()
{
    return ui->checkBox->checkState() == Qt::Checked;
}

void AdvancedConfigDialog::setCrop1(const bool value)
{
    ui->checkBox->setCheckState((value == true ? Qt::Checked : Qt::Unchecked)) ;
}

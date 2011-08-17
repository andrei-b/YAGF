#include "popplerdialog.h"
#include "ui_popplerdialog.h"
#include <QFileDialog>

PopplerDialog::PopplerDialog(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::PopplerDialog)
{
    ui->setupUi(this);
}

PopplerDialog::~PopplerDialog()
{
    delete ui;
}

void PopplerDialog::on_checkBox_toggled(bool checked)
{
    ui->spinBox->setEnabled(!checked);
    ui->spinBox_2->setEnabled(!checked);
}

void PopplerDialog::on_pushButton_clicked()
{
    QString fileName = QFileDialog::getOpenFileName(this,
         trUtf8("Open PDF File"), "", trUtf8("PDF Files (*.pdf)"));
    if (!fileName.isEmpty())
        ui->lineEdit->setText(fileName);
}

#ifndef POPPLERDIALOG_H
#define POPPLERDIALOG_H

#include <QDialog>

namespace Ui {
    class PopplerDialog;
}

class PopplerDialog : public QDialog
{
    Q_OBJECT

public:
    explicit PopplerDialog(QWidget *parent = 0);
    ~PopplerDialog();

private slots:
    void on_checkBox_toggled(bool checked);

    void on_pushButton_clicked();

private:
    Ui::PopplerDialog *ui;
};

#endif // POPPLERDIALOG_H

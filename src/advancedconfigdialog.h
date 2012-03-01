#ifndef ADVANCEDCONFIGDIALOG_H
#define ADVANCEDCONFIGDIALOG_H

#include <QDialog>

namespace Ui {
    class AdvancedConfigDialog;
}

class AdvancedConfigDialog : public QDialog
{
    Q_OBJECT

public:
    explicit AdvancedConfigDialog(QWidget *parent = 0);
    ~AdvancedConfigDialog();
    bool doCrop1();
    void setCrop1(const bool value);
private:
    Ui::AdvancedConfigDialog *ui;
};

#endif // ADVANCEDCONFIGDIALOG_H

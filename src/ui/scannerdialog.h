#pragma once

#include <QDialog>
#include <QProcess>

class QComboBox;
class QLabel;
class QPushButton;
class QSpinBox;

namespace yagf::ui {

class ScannerDialog : public QDialog {
    Q_OBJECT
public:
    explicit ScannerDialog(QString destination, QWidget *parent = nullptr);
    ~ScannerDialog() override;

private:
    void discover();
    void startScan();
    void finishScan(int exitCode, QProcess::ExitStatus status);

    QString destination;
    QProcess discovery;
    QProcess scan;
    QComboBox *devices;
    QComboBox *mode;
    QSpinBox *resolution;
    QLabel *message;
    QPushButton *refreshButton;
    QPushButton *scanButton;
};

} // namespace yagf::ui

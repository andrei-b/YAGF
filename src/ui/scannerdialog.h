#pragma once

#include <QDialog>
#include <QProcess>
#include <QTimer>

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
    void finishDiscovery(const QByteArray &addresses = {});
    void useCachedDevices();
    void startScan();
    void finishScan(int exitCode, QProcess::ExitStatus status);

    QString destination;
    QProcess discovery;
    QProcess addressDiscovery;
    QTimer addressTimeout;
    QByteArray pendingDevices;
    QProcess scan;
    QComboBox *devices;
    QComboBox *mode;
    QSpinBox *resolution;
    QLabel *message;
    QPushButton *refreshButton;
    QPushButton *scanButton;
};

} // namespace yagf::ui

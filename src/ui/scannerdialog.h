#pragma once

#include "scansettings.h"

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
    explicit ScannerDialog(QString destination, const core::ScanSettings &defaults = {}, QWidget *parent = nullptr);
    core::ScanSettings scanSettings() const;
    ~ScannerDialog() override;

private:
    void discover();
    void finishDiscovery(const QByteArray &addresses = {});
    void useCachedDevices();
    void startScan();
    void finishScan(int exitCode, QProcess::ExitStatus status);

    QString destination;
    QString projectDevice;
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

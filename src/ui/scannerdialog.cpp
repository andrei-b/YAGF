#include "scannerdialog.h"

#include <QComboBox>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QImageReader>
#include <QPushButton>
#include <QSpinBox>
#include <QStandardPaths>
#include <QVBoxLayout>
#include <QSignalBlocker>
#include <QVector>
#include <utility>

namespace yagf::ui {
namespace {
struct ScannerDevice {QString id,label;};
struct ScannerSession {QVector<ScannerDevice> devices; QString selected; bool discovered=false;};
ScannerSession &scannerSession() {static ScannerSession state; return state;}
}

ScannerDialog::ScannerDialog(QString destination, QWidget *parent)
    : QDialog(parent), destination(std::move(destination))
{
    setWindowTitle(tr("Scan a page"));
    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout;
    devices = new QComboBox(this);
    devices->setObjectName("scannerDevices");
    form->addRow(tr("Scanner:"), devices);
    resolution = new QSpinBox(this);
    resolution->setRange(75, 1200);
    resolution->setSingleStep(50);
    resolution->setValue(300);
    resolution->setSuffix(tr(" DPI"));
    form->addRow(tr("Resolution:"), resolution);
    mode = new QComboBox(this);
    mode->addItem(tr("Scanner default"), QString());
    mode->addItem(tr("Color"), QStringLiteral("Color"));
    mode->addItem(tr("Grayscale"), QStringLiteral("Gray"));
    mode->addItem(tr("Black and white"), QStringLiteral("Lineart"));
    form->addRow(tr("Mode:"), mode);
    layout->addLayout(form);
    message = new QLabel(this);
    message->setWordWrap(true);
    layout->addWidget(message);
    auto *buttons = new QHBoxLayout;
    refreshButton = new QPushButton(tr("Refresh scanners"), this);
    refreshButton->setObjectName("refreshScanners");
    scanButton = new QPushButton(tr("Scan"), this);
    auto *cancelButton = new QPushButton(tr("Cancel"), this);
    buttons->addWidget(refreshButton);
    buttons->addStretch();
    buttons->addWidget(scanButton);
    buttons->addWidget(cancelButton);
    layout->addLayout(buttons);
    connect(refreshButton, &QPushButton::clicked, this, &ScannerDialog::discover);
    connect(devices,&QComboBox::currentIndexChanged,this,[this] {
        if (devices->currentIndex()>=0) scannerSession().selected=devices->currentData().toString();
    });
    connect(scanButton, &QPushButton::clicked, this, &ScannerDialog::startScan);
    connect(cancelButton, &QPushButton::clicked, this, [this] {
        if (scan.state() != QProcess::NotRunning) scan.kill();
        reject();
    });
    connect(&discovery, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus status) {
        refreshButton->setEnabled(true);
        devices->setEnabled(true);
        if (status != QProcess::NormalExit || exitCode != 0) {
            scanButton->setEnabled(devices->count()>0);
            message->setText(tr("Scanner discovery failed: %1")
                .arg(QString::fromUtf8(discovery.readAllStandardError()).trimmed()));
            return;
        }
        const QStringList lines = QString::fromUtf8(discovery.readAllStandardOutput())
            .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        QVector<ScannerDevice> found;
        for (const QString &line : lines) {
            const QStringList parts = line.split(QLatin1Char('\t'));
            const QString id = parts.first().trimmed();
            if (!id.isEmpty()) found.append({id,parts.size()>1 ? QStringLiteral("%1 (%2)").arg(parts.at(1).trimmed(),id) : id});
        }
        auto &session=scannerSession(); session.devices=std::move(found); session.discovered=true;
        useCachedDevices();
    });
    connect(&discovery, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            refreshButton->setEnabled(true);
            devices->setEnabled(true);
            scanButton->setEnabled(devices->count()>0);
            message->setText(tr("Could not start scanimage."));
        }
    });
    connect(&scan, &QProcess::finished, this, &ScannerDialog::finishScan);
    connect(&scan, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            scanButton->setEnabled(true);
            refreshButton->setEnabled(true);
            devices->setEnabled(true);
            message->setText(tr("Could not start scanimage."));
        }
    });
    if (scannerSession().discovered) useCachedDevices(); else discover();
}

void ScannerDialog::useCachedDevices()
{
    auto &session=scannerSession();
    const QSignalBlocker block(devices);
    devices->clear();
    for (const auto &device:session.devices) devices->addItem(device.label,device.id);
    const int selected=devices->findData(session.selected);
    if (selected>=0) devices->setCurrentIndex(selected);
    session.selected=devices->currentData().toString();
    scanButton->setEnabled(devices->count()>0);
    message->setText(devices->count() ? tr("Ready to scan one page. Refresh scanners if devices have changed.")
        : tr("No scanner found. Check its connection and SANE driver, then refresh."));
}

ScannerDialog::~ScannerDialog()
{
    for (QProcess *process : {&discovery, &scan}) {
        if (process->state() != QProcess::NotRunning) {
            process->kill();
            process->waitForFinished(2000);
        }
    }
    if (result() != QDialog::Accepted) QFile::remove(destination);
}

void ScannerDialog::discover()
{
    if (discovery.state() != QProcess::NotRunning || scan.state() != QProcess::NotRunning) return;
    devices->setEnabled(false);
    scanButton->setEnabled(false);
    refreshButton->setEnabled(false);
    message->setText(tr("Looking for scanners…"));
    if (QStandardPaths::findExecutable(QStringLiteral("scanimage")).isEmpty()) {
        refreshButton->setEnabled(true);
        devices->setEnabled(true);
        scanButton->setEnabled(devices->count()>0);
        message->setText(tr("Install SANE's scanimage command to use a scanner."));
        return;
    }
    discovery.start(QStringLiteral("scanimage"),
        {QStringLiteral("--formatted-device-list=%d\t%v %m%n")});
}

void ScannerDialog::startScan()
{
    if (devices->currentIndex() < 0 || scan.state() != QProcess::NotRunning) return;
    QFile::remove(destination);
    QStringList arguments{QStringLiteral("--device-name=%1").arg(devices->currentData().toString()),
                          QStringLiteral("--format=png"),
                          QStringLiteral("--resolution=%1").arg(resolution->value()),
                          QStringLiteral("--output-file=%1").arg(destination)};
    if (!mode->currentData().toString().isEmpty())
        arguments << QStringLiteral("--mode=%1").arg(mode->currentData().toString());
    scanButton->setEnabled(false);
    refreshButton->setEnabled(false);
    devices->setEnabled(false);
    message->setText(tr("Scanning…"));
    scan.start(QStringLiteral("scanimage"), arguments);
}

void ScannerDialog::finishScan(int exitCode, QProcess::ExitStatus status)
{
    if (status == QProcess::NormalExit && exitCode == 0 && QFileInfo(destination).size() > 0 &&
        QImageReader(destination).canRead()) {
        accept();
        return;
    }
    QFile::remove(destination);
    scanButton->setEnabled(devices->count() > 0);
    refreshButton->setEnabled(true);
    devices->setEnabled(true);
    message->setText(tr("Scanning failed: %1")
        .arg(QString::fromUtf8(scan.readAllStandardError()).trimmed()));
}

} // namespace yagf::ui

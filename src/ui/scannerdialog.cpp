#include "scannerdialog.h"

#include <QComboBox>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QImageReader>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QStandardPaths>
#include <QVBoxLayout>
#include <QSignalBlocker>
#include <QVector>
#include <QUrl>
#include <utility>

namespace yagf::ui {
namespace {
struct ScannerDevice {QString id,label;};
struct ScannerSession {QVector<ScannerDevice> devices; QString selected; bool discovered=false;};
ScannerSession &scannerSession() {static ScannerSession state; return state;}

QRegularExpressionMatch airscanIdentity(const QString &id)
{
    // AirScan's hexadecimal device index belongs to one scanimage process only.
    static const QRegularExpression pattern(QStringLiteral("^airscan:([ew])[0-9a-f]+:(.+)$"));
    return pattern.match(id);
}

QString airscanAddress(const QString &id, const QByteArray &addresses)
{
    const auto identity = airscanIdentity(id);
    if (!identity.hasMatch()) return id;
    const QString protocol = identity.captured(1) == QLatin1String("e") ? QStringLiteral("escl") : QStringLiteral("wsd");
    const QString name = identity.captured(2);
    bool inDevices = false;
    for (const QString &raw : QString::fromUtf8(addresses).split(QLatin1Char('\n'))) {
        const QString line = raw.trimmed();
        if (line.startsWith(QLatin1Char('['))) {
            inDevices = line.compare(QLatin1String("[devices]"), Qt::CaseInsensitive) == 0;
            continue;
        }
        if (!inDevices || line.startsWith(QLatin1Char('#')) || line.startsWith(QLatin1Char(';'))) continue;
        // Names may be quoted and URLs may contain '=' or IPv6 colons.
        const int equals = line.indexOf(QLatin1Char('='), line.startsWith(QLatin1Char('"')) ? line.indexOf(QLatin1Char('"'), 1) + 1 : 0);
        if (equals < 0) continue;
        QString discoveredName = line.left(equals).trimmed();
        if (discoveredName.startsWith(QLatin1Char('"')) && discoveredName.endsWith(QLatin1Char('"')))
            discoveredName = discoveredName.mid(1, discoveredName.size() - 2);
        if (discoveredName != name) continue;
        const QString value = line.mid(equals + 1).trimmed();
        const int comma = value.lastIndexOf(QLatin1Char(','));
        const QString discoveredProtocol = comma < 0 ? QStringLiteral("escl") : value.mid(comma + 1).trimmed().toLower();
        if (discoveredProtocol != protocol) continue;
        const QString address = (comma < 0 ? value : value.left(comma)).trimmed();
        const QUrl url(address, QUrl::StrictMode);
        if (!url.isValid() || url.host().isEmpty() ||
            (url.scheme() != QLatin1String("http") && url.scheme() != QLatin1String("https"))) continue;
        // The native backend syntax is protocol:name:URL, so its name cannot contain ':'.
        const QString alias = name.contains(QLatin1Char(':')) ? QStringLiteral("YAGF scanner") : name;
        return QStringLiteral("airscan:%1:%2:%3").arg(protocol, alias, address);
    }
    return id;
}
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
        addressTimeout.stop();
        if (addressDiscovery.state() != QProcess::NotRunning) addressDiscovery.kill();
        if (scan.state() != QProcess::NotRunning) scan.kill();
        reject();
    });
    connect(&discovery, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus status) {
        if (status != QProcess::NormalExit || exitCode != 0) {
            refreshButton->setEnabled(true);
            devices->setEnabled(true);
            scanButton->setEnabled(devices->count()>0);
            message->setText(tr("Scanner discovery failed: %1")
                .arg(QString::fromUtf8(discovery.readAllStandardError()).trimmed()));
            return;
        }
        pendingDevices = discovery.readAllStandardOutput();
        bool needsAddresses = false;
        for (const QByteArray &line : pendingDevices.split('\n')) {
            if (airscanIdentity(QString::fromUtf8(line.split('\t').first()).trimmed()).hasMatch()) {
                needsAddresses = true;
                break;
            }
        }
        const QString resolver = QStandardPaths::findExecutable(QStringLiteral("airscan-discover"));
        if (!needsAddresses || resolver.isEmpty()) {finishDiscovery(); return;}
        message->setText(tr("Resolving network scanner addresses…"));
        addressTimeout.start(10000);
        addressDiscovery.start(resolver, QStringList{});
    });
    addressTimeout.setSingleShot(true);
    connect(&addressTimeout, &QTimer::timeout, &addressDiscovery, &QProcess::kill);
    connect(&addressDiscovery, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        addressTimeout.stop();
        const QByteArray addresses = addressDiscovery.readAllStandardOutput();
        addressDiscovery.readAllStandardError();
        finishDiscovery(status == QProcess::NormalExit && code == 0 ? addresses : QByteArray{});
    });
    connect(&addressDiscovery, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {addressTimeout.stop(); finishDiscovery();}
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

void ScannerDialog::finishDiscovery(const QByteArray &addresses)
{
    QVector<ScannerDevice> found;
    bool unresolved = false;
    for (const QByteArray &line : pendingDevices.split('\n')) {
        const QStringList parts = QString::fromUtf8(line).split(QLatin1Char('\t'));
        const QString original = parts.first().trimmed();
        if (original.isEmpty()) continue;
        const QString id = airscanAddress(original, addresses);
        unresolved |= airscanIdentity(id).hasMatch();
        found.append({id, parts.size() > 1 ? QStringLiteral("%1 (%2)").arg(parts.at(1).trimmed(), id) : id});
    }
    pendingDevices.clear();
    auto &session = scannerSession();
    // A failed optional resolver must not replace a working connection with a
    // process-local index. Unresolved legacy IDs can still be tried this time,
    // but are never treated as reusable across scan windows.
    if (!unresolved || !session.discovered) {
        session.devices = std::move(found);
        session.discovered = !unresolved;
    }
    refreshButton->setEnabled(true);
    devices->setEnabled(true);
    useCachedDevices();
    if (unresolved)
        message->setText(tr("Could not resolve a network scanner address. Check airscan-discover and refresh scanners if opening the scanner fails."));
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
    addressTimeout.stop();
    // Disconnect completion handlers before killing children during destruction.
    for (QProcess *process : {&discovery, &addressDiscovery, &scan}) {
        disconnect(process, nullptr, this, nullptr);
        if (process->state() != QProcess::NotRunning) {
            process->kill();
            process->waitForFinished(2000);
        }
    }
    if (result() != QDialog::Accepted) QFile::remove(destination);
}

void ScannerDialog::discover()
{
    if (discovery.state() != QProcess::NotRunning || addressDiscovery.state() != QProcess::NotRunning || scan.state() != QProcess::NotRunning) return;
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
    if (devices->currentIndex() < 0 || discovery.state() != QProcess::NotRunning ||
        addressDiscovery.state() != QProcess::NotRunning || scan.state() != QProcess::NotRunning) return;
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

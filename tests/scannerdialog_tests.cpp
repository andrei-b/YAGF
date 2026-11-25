#include "scannerdialog.h"

#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QPushButton>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QThread>
#include <functional>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    const auto wait=[](const std::function<bool()> &ready, int timeout = 4000) {
        QElapsedTimer timer;timer.start();
        while (!ready() && timer.elapsed()<timeout) {QApplication::processEvents();QThread::msleep(5);}
        assert(ready());
    };
    if (argc>1 && QString::fromLocal8Bit(argv[1])=="--fresh-session-probe") {
        QTemporaryDir fresh; yagf::ui::ScannerDialog dialog(fresh.filePath("page.png"));
        auto *devices=dialog.findChild<QComboBox *>("scannerDevices");
        wait([&]{return devices->count()>0;});
        return 0;
    }
    QTemporaryDir directory;
    assert(directory.isValid());
    const QString scriptPath = directory.filePath(QStringLiteral("scanimage"));
    const QString sourcePath = directory.filePath(QStringLiteral("fixture.png"));
    const auto listPath=directory.filePath("devices.txt"), logPath=directory.filePath("discoveries.txt"),argsPath=directory.filePath("scan-args.txt");
    const auto deviceList=[&](const QByteArray &text) {QFile list(listPath);assert(list.open(QIODevice::WriteOnly));assert(list.write(text)==text.size());};
    deviceList("virtual:0\tFirst Scanner\nvirtual:1\tSecond Scanner\n");
    qputenv("TEST_SCAN_DEVICES",listPath.toUtf8());qputenv("TEST_SCAN_DISCOVERY_LOG",logPath.toUtf8());qputenv("TEST_SCAN_ARGS",argsPath.toUtf8());
    const auto probes=[&] {QFile log(logPath);return log.open(QIODevice::ReadOnly) ? log.readAll().count('\n') : 0;};
    QImage fixture(24, 32, QImage::Format_RGB32);
    fixture.fill(Qt::white);
    assert(fixture.save(sourcePath));
    QFile script(scriptPath);
    assert(script.open(QIODevice::WriteOnly));
    const QByteArray body =
        "#!/bin/sh\n"
        "case \"$1\" in\n"
        "  --formatted-device-list=*) echo probe >> \"$TEST_SCAN_DISCOVERY_LOG\";\n"
        "    if [ -n \"$TEST_SCAN_DISCOVERY_FAIL\" ]; then echo 'discovery failure fixture' >&2; exit 7; fi\n"
        "    cat \"$TEST_SCAN_DEVICES\"; exit 0;;\n"
        "esac\n"
        "printf '%s\\n' \"$@\" > \"$TEST_SCAN_ARGS\"\n"
        // Enumeration deliberately returns an ID that a new scan process cannot open.
        "case \"$1\" in\n"
        "  --device-name=airscan:[ew][0-9a-f]*:*) echo 'open of device failed: Invalid argument' >&2; exit 1;;\n"
        "esac\n"
        "for arg do\n"
        "  case \"$arg\" in\n"
        "    --output-file=*)\n"
        "      if [ -n \"$TEST_SCAN_BAD\" ]; then printf invalid > \"${arg#*=}\"; exit 0; fi\n"
        "      cp \"$TEST_SCAN_IMAGE\" \"${arg#*=}\"; exit $?;;\n"
        "  esac\n"
        "done\n"
        "exit 1\n";
    assert(script.write(body) == body.size());
    script.close();
    assert(script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                 QFileDevice::ExeOwner));
    qputenv("PATH", QFile::encodeName(directory.path()) + ':' + qgetenv("PATH"));
    qputenv("TEST_SCAN_IMAGE", QFile::encodeName(sourcePath));

    const QString scannedPath = directory.filePath(QStringLiteral("scanned.png"));
    yagf::ui::ScannerDialog dialog(scannedPath);
    const auto combos = dialog.findChildren<QComboBox *>();
    assert(combos.size() == 2);
    QElapsedTimer timer;
    timer.start();
    while (combos.first()->count() == 0 && timer.elapsed() < 3000) {
        QApplication::processEvents();
        QThread::msleep(10);
    }
    assert(combos.first()->count() == 2);
    assert(combos.first()->currentData().toString() == QStringLiteral("virtual:0"));
    combos.first()->setCurrentIndex(1);
    assert(probes()==1);
    QPushButton *scanButton = nullptr;
    for (QPushButton *button : dialog.findChildren<QPushButton *>())
        if (button->text() == QStringLiteral("Scan")) scanButton = button;
    assert(scanButton && scanButton->isEnabled());
    combos.at(1)->setCurrentIndex(combos.at(1)->findData("Gray"));
    dialog.findChild<QSpinBox *>("scannerResolution")->setValue(600);
    scanButton->click();
    assert(!combos.at(1)->isEnabled() && !dialog.findChild<QSpinBox *>("scannerResolution")->isEnabled());
    timer.restart();
    while (dialog.result() != QDialog::Accepted && timer.elapsed() < 3000) {
        QApplication::processEvents();
        QThread::msleep(10);
    }
    assert(dialog.result() == QDialog::Accepted);
    assert(QImage(scannedPath).size() == fixture.size());
    {QFile args(argsPath);assert(args.open(QIODevice::ReadOnly));const auto arguments = args.readAll();
        assert(arguments.contains("--device-name=virtual:1\n") && arguments.contains("--mode=Gray\n") &&
               arguments.contains("--resolution=600\n"));}

    qputenv("TEST_SCAN_BAD", "1");
    const QString badPath = directory.filePath(QStringLiteral("bad.png"));
    yagf::ui::ScannerDialog badDialog(badPath);
    const auto badCombos = badDialog.findChildren<QComboBox *>();
    timer.restart();
    while (badCombos.first()->count() == 0 && timer.elapsed() < 3000) {
        QApplication::processEvents();
        QThread::msleep(10);
    }
    assert(badCombos.first()->count() == 2);
    assert(badCombos.first()->currentData().toString()=="virtual:1");
    assert(probes()==1); // Opening another scan window never re-enumerates devices.
    QPushButton *badScanButton = nullptr;
    for (QPushButton *button : badDialog.findChildren<QPushButton *>())
        if (button->text() == QStringLiteral("Scan")) badScanButton = button;
    assert(badScanButton && badScanButton->isEnabled());
    badScanButton->click();
    timer.restart();
    while (!badScanButton->isEnabled() && timer.elapsed() < 3000) {
        QApplication::processEvents();
        QThread::msleep(10);
    }
    assert(badScanButton->isEnabled());
    assert(badDialog.result() != QDialog::Accepted);
    assert(!QFile::exists(badPath));

    // A failed scan also preserves the device choice for the next window.
    yagf::ui::ScannerDialog cached(directory.filePath("cached.png"));
    auto *devices=cached.findChild<QComboBox *>("scannerDevices");auto *refresh=cached.findChild<QPushButton *>("refreshScanners");
    assert(devices->count()==2 && devices->currentData().toString()=="virtual:1" && probes()==1);

    // Refresh replaces the list and restores selection by ID, not row number.
    deviceList("virtual:1\tSecond Scanner\nvirtual:0\tFirst Scanner\n");
    refresh->click();wait([&]{return refresh->isEnabled() && probes()==2;});
    assert(devices->count()==2 && devices->currentIndex()==0 && devices->currentData().toString()=="virtual:1");
    deviceList("virtual:2\tReplacement Scanner\n");
    refresh->click();wait([&]{return refresh->isEnabled() && probes()==3;});
    assert(devices->count()==1 && devices->currentData().toString()=="virtual:2");

    // An absent project device needs an explicit replacement. Its resolution and mode still apply.
    const auto settings = dialog.scanSettings();
    yagf::ui::ScannerDialog missing(directory.filePath("missing.png"), settings);
    auto *choice = missing.findChild<QComboBox *>("scannerDevices");
    QPushButton *missingScan = nullptr;
    for (auto *button : missing.findChildren<QPushButton *>())
        if (button->text() == "Scan") missingScan = button;
    assert(choice->currentIndex() < 0 && missingScan && !missingScan->isEnabled());
    choice->setCurrentIndex(0);
    assert(missingScan->isEnabled() && missing.scanSettings().device == "virtual:2" &&
           missing.scanSettings().dpi == 600 && missing.scanSettings().mode == "Gray");

    // An unsuccessful refresh must not destroy the last known list/selection.
    qputenv("TEST_SCAN_DISCOVERY_FAIL","1");refresh->click();wait([&]{return refresh->isEnabled() && probes()==4;});
    assert(devices->count()==1 && devices->currentData().toString()=="virtual:2");
    yagf::ui::ScannerDialog afterFailure(directory.filePath("after-failure.png"));
    assert(afterFailure.findChild<QComboBox *>("scannerDevices")->currentData().toString()=="virtual:2" && probes()==4);

    // A successful empty result is cached too; manual Refresh discovers devices added later.
    qunsetenv("TEST_SCAN_DISCOVERY_FAIL");deviceList({});refresh->click();wait([&]{return refresh->isEnabled() && probes()==5;});
    assert(devices->count()==0);
    yagf::ui::ScannerDialog empty(directory.filePath("empty.png"));QApplication::processEvents();
    assert(empty.findChild<QComboBox *>("scannerDevices")->count()==0 && probes()==5);
    deviceList("virtual:3\tNew Scanner\n");auto *emptyRefresh=empty.findChild<QPushButton *>("refreshScanners");emptyRefresh->click();wait([&]{return emptyRefresh->isEnabled() && probes()==6;});
    assert(empty.findChild<QComboBox *>("scannerDevices")->currentData().toString()=="virtual:3");

    // A new application process must enumerate afresh: this is a session cache.
    QProcess fresh;fresh.start(QCoreApplication::applicationFilePath(),{"--fresh-session-probe"});
    assert(fresh.waitForFinished(5000) && fresh.exitCode()==0 && fresh.exitStatus()==QProcess::NormalExit);
    assert(probes()==7);

    const auto addressPath = directory.filePath("addresses.txt"), addressLog = directory.filePath("address-probes.txt");
    const auto addressList = [&](const QByteArray &text) {QFile file(addressPath);assert(file.open(QIODevice::WriteOnly));assert(file.write(text)==text.size());};
    const auto addressProbes = [&] {QFile file(addressLog);return file.open(QIODevice::ReadOnly) ? file.readAll().count('\n') : 0;};
    qputenv("TEST_SCAN_ADDRESSES", addressPath.toUtf8());
    qputenv("TEST_SCAN_ADDRESS_LOG", addressLog.toUtf8());
    QFile resolver(directory.filePath("airscan-discover"));
    assert(resolver.open(QIODevice::WriteOnly));
    const QByteArray resolverBody =
        "#!/bin/sh\n"
        "echo probe >> \"$TEST_SCAN_ADDRESS_LOG\"\n"
        "if [ -n \"$TEST_SCAN_ADDRESS_FAIL\" ]; then echo 'resolver failure fixture' >&2; exit 1; fi\n"
        "if [ -n \"$TEST_SCAN_ADDRESS_SLOW\" ]; then exec sleep 30; fi\n"
        "cat \"$TEST_SCAN_ADDRESSES\"\n";
    assert(resolver.write(resolverBody)==resolverBody.size());resolver.close();
    assert(resolver.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    addressList("[devices]\n"
                "Other scanner = http://192.0.2.99/eSCL/, eSCL\n"
                "Brother Scanner = http://192.0.2.13/WebServices/ScannerService, WSD\n"
                "Brother Scanner = http://192.0.2.13/eSCL/, eSCL\n"
                "Brother Scanner = http://[fe80::1%253]/eSCL/, eSCL\n");
    deviceList("airscan:e1b:Brother Scanner\teSCL Brother Scanner\nairscan:w3:Brother Scanner\tWSD Brother Scanner\n");
    qunsetenv("TEST_SCAN_BAD");
    emptyRefresh->click();wait([&]{return emptyRefresh->isEnabled() && probes()==8;});
    auto *networkDevices=empty.findChild<QComboBox *>("scannerDevices");
    const QString escl = QStringLiteral("airscan:escl:Brother Scanner:http://192.0.2.13/eSCL/");
    const QString wsd = QStringLiteral("airscan:wsd:Brother Scanner:http://192.0.2.13/WebServices/ScannerService");
    assert(networkDevices->count()==2 && networkDevices->itemData(0).toString()==escl && networkDevices->itemData(1).toString()==wsd);
    assert(addressProbes()==1);

    const auto scanPage = [&](const QString &name, const QString &expected) {
        const auto path=directory.filePath(name);
        yagf::ui::ScannerDialog next(path);
        auto *combo=next.findChild<QComboBox *>("scannerDevices");
        assert(combo->currentData().toString()==expected);
        QPushButton *button=nullptr;
        for (auto *candidate:next.findChildren<QPushButton *>())
            if (candidate->text()==QStringLiteral("Scan")) button=candidate;
        assert(button && button->isEnabled());button->click();
        wait([&]{return next.result()==QDialog::Accepted || button->isEnabled();});
        assert(next.result()==QDialog::Accepted && QImage(path).size()==fixture.size());
        QFile args(argsPath);assert(args.open(QIODevice::ReadOnly));
        assert(args.readAll().contains(("--device-name="+expected+'\n').toUtf8()));
    };
    // Separate scanimage processes must work with the same remembered scanner.
    scanPage("airscan-first.png",escl);scanPage("airscan-second.png",escl);
    assert(probes()==8 && addressProbes()==1);

    // Renumbering and reordering cannot change which scanner/protocol was selected.
    networkDevices->setCurrentIndex(1);
    const auto networkSettings = empty.scanSettings();
    deviceList("airscan:w0:Brother Scanner\tWSD Brother Scanner\nairscan:e2:Brother Scanner\teSCL Brother Scanner\n");
    emptyRefresh->click();wait([&]{return emptyRefresh->isEnabled() && probes()==9;});
    assert(networkDevices->currentIndex()==0 && networkDevices->currentData().toString()==wsd && addressProbes()==2);
    scanPage("airscan-wsd.png",wsd);
    networkDevices->setCurrentIndex(1); // A different device was used in the session.
    yagf::ui::ScannerDialog restored(directory.filePath("restored.png"), networkSettings);
    assert(restored.findChild<QComboBox *>("scannerDevices")->currentData().toString() == wsd);
    networkDevices->setCurrentIndex(0);

    // Resolution failure must preserve a usable cached address rather than its new temporary ID.
    qputenv("TEST_SCAN_ADDRESS_FAIL","1");
    emptyRefresh->click();wait([&]{return emptyRefresh->isEnabled() && probes()==10;});
    assert(networkDevices->currentData().toString()==wsd && addressProbes()==3);
    scanPage("airscan-after-resolver-failure.png",wsd);
    qunsetenv("TEST_SCAN_ADDRESS_FAIL");

    // Names with '=' or ':' and IPv6 endpoints must reach scanimage as one argument.
    deviceList("airscan:e9:Lab: Scanner = Alpha\teSCL Lab scanner\n");
    addressList("[devices]\n\"Lab: Scanner = Alpha\" = https://[2001:db8::1]:8443/custom/eSCL/?key=a=b, eSCL\n");
    emptyRefresh->click();wait([&]{return emptyRefresh->isEnabled() && probes()==11;});
    const QString ipv6=QStringLiteral("airscan:escl:YAGF scanner:https://[2001:db8::1]:8443/custom/eSCL/?key=a=b");
    assert(networkDevices->currentData().toString()==ipv6);
    scanPage("airscan-ipv6.png",ipv6);

    // A stalled optional helper must release the window and preserve the last working device.
    qputenv("TEST_SCAN_ADDRESS_SLOW","1");
    emptyRefresh->click();wait([&]{return emptyRefresh->isEnabled() && probes()==12;},15000);
    assert(networkDevices->currentData().toString()==ipv6);

    // Closing during address discovery must kill the child promptly and leave the cache intact.
    timer.restart();
    {
        yagf::ui::ScannerDialog closing(directory.filePath("closing.png"));
        closing.findChild<QPushButton *>("refreshScanners")->click();
        wait([&]{return addressProbes()==6;});
    }
    assert(timer.elapsed()<3000);
    qunsetenv("TEST_SCAN_ADDRESS_SLOW");
    yagf::ui::ScannerDialog afterCancel(directory.filePath("after-cancel.png"));
    assert(afterCancel.findChild<QComboBox *>("scannerDevices")->currentData().toString()==ipv6);
}

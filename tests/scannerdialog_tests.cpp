#include "scannerdialog.h"

#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QPushButton>
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
    const auto wait=[](const std::function<bool()> &ready) {
        QElapsedTimer timer;timer.start();
        while (!ready() && timer.elapsed()<4000) {QApplication::processEvents();QThread::msleep(5);}
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
    scanButton->click();
    timer.restart();
    while (dialog.result() != QDialog::Accepted && timer.elapsed() < 3000) {
        QApplication::processEvents();
        QThread::msleep(10);
    }
    assert(dialog.result() == QDialog::Accepted);
    assert(QImage(scannedPath).size() == fixture.size());
    {QFile args(argsPath);assert(args.open(QIODevice::ReadOnly));assert(args.readAll().contains("--device-name=virtual:1\n"));}

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
}

#include "scannerdialog.h"

#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QPushButton>
#include <QTemporaryDir>
#include <QThread>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QTemporaryDir directory;
    assert(directory.isValid());
    const QString scriptPath = directory.filePath(QStringLiteral("scanimage"));
    const QString sourcePath = directory.filePath(QStringLiteral("fixture.png"));
    QImage fixture(24, 32, QImage::Format_RGB32);
    fixture.fill(Qt::white);
    assert(fixture.save(sourcePath));
    QFile script(scriptPath);
    assert(script.open(QIODevice::WriteOnly));
    const QByteArray body =
        "#!/bin/sh\n"
        "case \"$1\" in\n"
        "  --formatted-device-list=*) printf 'virtual:0\\tVirtual Scanner\\n'; exit 0;;\n"
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
        app.processEvents();
        QThread::msleep(10);
    }
    assert(combos.first()->count() == 1);
    assert(combos.first()->currentData().toString() == QStringLiteral("virtual:0"));
    QPushButton *scanButton = nullptr;
    for (QPushButton *button : dialog.findChildren<QPushButton *>())
        if (button->text() == QStringLiteral("Scan")) scanButton = button;
    assert(scanButton && scanButton->isEnabled());
    scanButton->click();
    timer.restart();
    while (dialog.result() != QDialog::Accepted && timer.elapsed() < 3000) {
        app.processEvents();
        QThread::msleep(10);
    }
    assert(dialog.result() == QDialog::Accepted);
    assert(QImage(scannedPath).size() == fixture.size());

    qputenv("TEST_SCAN_BAD", "1");
    const QString badPath = directory.filePath(QStringLiteral("bad.png"));
    yagf::ui::ScannerDialog badDialog(badPath);
    const auto badCombos = badDialog.findChildren<QComboBox *>();
    timer.restart();
    while (badCombos.first()->count() == 0 && timer.elapsed() < 3000) {
        app.processEvents();
        QThread::msleep(10);
    }
    assert(badCombos.first()->count() == 1);
    QPushButton *badScanButton = nullptr;
    for (QPushButton *button : badDialog.findChildren<QPushButton *>())
        if (button->text() == QStringLiteral("Scan")) badScanButton = button;
    assert(badScanButton && badScanButton->isEnabled());
    badScanButton->click();
    timer.restart();
    while (!badScanButton->isEnabled() && timer.elapsed() < 3000) {
        app.processEvents();
        QThread::msleep(10);
    }
    assert(badScanButton->isEnabled());
    assert(badDialog.result() != QDialog::Accepted);
    assert(!QFile::exists(badPath));
}

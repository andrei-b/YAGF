#include "preprocessdialog.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QPushButton>
#include <QTemporaryDir>
#include <QFile>
#include <QPainter>
#include <QTransform>
#include <QtTest>
using namespace yagf::ui;
class PreprocessDialogTests : public QObject {
    Q_OBJECT
private slots:
    void settingsAndOrientation() {
        QTemporaryDir dir;QVERIFY(dir.isValid());QFile helper(dir.filePath("tesseract"));QVERIFY(helper.open(QIODevice::WriteOnly));
        helper.write("#!/bin/sh\nprintf 'Rotate: 270\\nOrientation confidence: 20.0\\n'\n");helper.close();QVERIFY(helper.setPermissions(QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
        const QByteArray oldPath=qgetenv("PATH");qputenv("PATH",QFile::encodeName(dir.path())+':'+oldPath);
        QImage image(600,400,QImage::Format_RGB32);image.fill(Qt::white);
        QPainter painter(&image);for(int y=40;y<350;y+=35)for(int x=40;x<550;x+=40)painter.fillRect(x,y,25,8,Qt::black);painter.end();
        PreprocessDialog dialog(image,{.quarterTurns=1,.adaptiveBinarization=true});
        auto *rotation=dialog.findChild<QComboBox *>("pageRotation");auto *binary=dialog.findChild<QCheckBox *>("adaptiveBinarization");
        auto *angle=dialog.findChild<QDoubleSpinBox *>("deskewAngle");QVERIFY(rotation && binary && angle);
        QCOMPARE(dialog.settings().quarterTurns,1);QVERIFY(dialog.settings().adaptiveBinarization);
        rotation->setCurrentIndex(0);angle->setValue(3);binary->setChecked(false);QCOMPARE(dialog.settings().deskewDegrees,3.0);QVERIFY(!dialog.settings().adaptiveBinarization);
        QPushButton *detect=nullptr;for(auto *button:dialog.findChildren<QPushButton *>())if(button->text()=="Detect orientation + skew")detect=button;
        QVERIFY(detect);detect->click();QTRY_VERIFY_WITH_TIMEOUT(detect->isEnabled(),5000);QCOMPARE(dialog.settings().quarterTurns,3);
        qputenv("PATH",oldPath);
    }
};
QTEST_MAIN(PreprocessDialogTests)
#include "preprocessdialog_tests.moc"

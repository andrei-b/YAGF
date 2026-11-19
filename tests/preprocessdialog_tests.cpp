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
    void textGuidedSettings() {
        QTemporaryDir dir;
        QFile helper(dir.filePath("tesseract"));
        QVERIFY(helper.open(QIODevice::WriteOnly));
        QByteArray tsv("level\tpage_num\tblock_num\tpar_num\tline_num\tword_num\tleft\ttop\twidth\theight\tconf\ttext\n");
        QImage image(600,400,QImage::Format_RGB32);
        image.fill(QColor(195,195,195));
        QPainter painter(&image);
        for (int line=0; line<3; ++line) {
            for (int word=0; word<5; ++word) {
                const int x=35+100*word, y=50+100*line+5*word;
                for (int offset=1; offset<30; offset+=6)
                    painter.fillRect(x+offset,y,2,13,QColor(85,85,85));
                tsv += QString("5\t1\t1\t1\t%1\t%2\t%3\t%4\t30\t13\t96\ttext\n")
                    .arg(line+1).arg(word+1).arg(x).arg(y).toUtf8();
            }
        }
        painter.end();
        helper.write("#!/bin/sh\ncat <<'TSV'\n"+tsv+"TSV\n");
        helper.close();
        QVERIFY(helper.setPermissions(QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
        const QByteArray oldPath=qgetenv("PATH");
        qputenv("PATH",QFile::encodeName(dir.path())+':'+oldPath);
        PreprocessDialog dialog(image,{});
        auto *button=dialog.findChild<QPushButton *>("analyzeText");
        QVERIFY(button);
        button->click();
        QTRY_VERIFY_WITH_TIMEOUT(dialog.settings().adaptiveBinarization,5000);
        qputenv("PATH",oldPath);
        QVERIFY(std::abs(dialog.settings().deskewDegrees-2.86)<0.1);
        QVERIFY(dialog.settings().inkThreshold>10);
    }
    void settingsAndOrientation() {
        QTemporaryDir dir;QVERIFY(dir.isValid());QFile helper(dir.filePath("tesseract"));QVERIFY(helper.open(QIODevice::WriteOnly));
        helper.write("#!/bin/sh\nprintf 'Rotate: 270\\nOrientation confidence: 20.0\\n'\n");helper.close();QVERIFY(helper.setPermissions(QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
        const QByteArray oldPath=qgetenv("PATH");qputenv("PATH",QFile::encodeName(dir.path())+':'+oldPath);
        QImage image(600,400,QImage::Format_RGB32);image.fill(Qt::white);
        QPainter painter(&image);for(int y=40;y<350;y+=35)for(int x=40;x<550;x+=40)painter.fillRect(x,y,25,8,Qt::black);painter.end();
        PreprocessDialog dialog(image,{.quarterTurns=1,.adaptiveBinarization=true,.bendPercent=-2.5,.levelShadows=true});
        auto *rotation=dialog.findChild<QComboBox *>("pageRotation");auto *binary=dialog.findChild<QCheckBox *>("adaptiveBinarization");
        auto *angle=dialog.findChild<QDoubleSpinBox *>("deskewAngle");QVERIFY(rotation && binary && angle);
        QCOMPARE(dialog.settings().quarterTurns,1);QVERIFY(dialog.settings().adaptiveBinarization);
        QCOMPARE(dialog.settings().bendPercent, -2.5); QVERIFY(dialog.settings().levelShadows);
        rotation->setCurrentIndex(0); QCOMPARE(dialog.settings().bendPercent, 0.); angle->setValue(3);binary->setChecked(false);QCOMPARE(dialog.settings().deskewDegrees,3.0);QVERIFY(!dialog.settings().adaptiveBinarization);
        QPushButton *detect=nullptr;for(auto *button:dialog.findChildren<QPushButton *>())if(button->text()=="Detect orientation + skew")detect=button;
        QVERIFY(detect);detect->click();QTRY_VERIFY_WITH_TIMEOUT(detect->isEnabled(),5000);QCOMPARE(dialog.settings().quarterTurns,3);
        qputenv("PATH",oldPath);
    }
};
QTEST_MAIN(PreprocessDialogTests)
#include "preprocessdialog_tests.moc"

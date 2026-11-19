#include "experimentdialog.h"
#include "exportpage.h"
#include "projectfile.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QFileDialog>
#include <QFile>
#include <QJsonDocument>
#include <QLineEdit>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QPainter>
#include <QPdfWriter>
#include <QtTest>
using namespace yagf;
class ExperimentDialogTests:public QObject {
    Q_OBJECT
private slots:
    void verifyRunInspectAndSave(){
        QTemporaryDir dir;QVERIFY(dir.isValid());const QString source=dir.filePath("source.pdf");
        {QPdfWriter pdf(source);pdf.setResolution(150);pdf.setPageSize(QPageSize(QSizeF(240,240),QPageSize::Point));pdf.setPageMargins(QMarginsF(0,0,0,0));QPainter p(&pdf);p.drawText(20,40,"alpha");QVERIFY(pdf.newPage());p.drawText(20,40,"beta");}
        QFile script(dir.filePath("tesseract"));QVERIFY(script.open(QIODevice::WriteOnly));
        script.write("#!/bin/sh\nif [ \"$1\" = \"--version\" ]; then echo 'tesseract fixture'; exit 0; fi\nprintf 'level\\tpage_num\\tblock_num\\tpar_num\\tline_num\\tword_num\\tleft\\ttop\\twidth\\theight\\tconf\\ttext\\n5\\t1\\t1\\t1\\t1\\t1\\t20\\t20\\t60\\t20\\t99\\talpha\\n'\n");script.close();QVERIFY(script.setPermissions(QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));const QByteArray oldPath=qgetenv("PATH");qputenv("PATH",QFile::encodeName(dir.path())+':'+oldPath);
        core::OcrDocument doc;doc.sourcePath=source;doc.totalPages=2;
        // Use the pixel dimensions produced by the local PDF renderer.
        auto scan = processing::readExportPage(doc, 1, dir, false);
        QVERIFY(scan);
        for(int number:{1,2}){core::OcrPage p;p.number=number;p.width=p.sourceWidth=scan->width();p.height=p.sourceHeight=scan->height();p.engineId="fixture";p.language="eng";core::OcrWord w;w.page=number;w.bounds={20,20,60,20};w.text=w.originalText=number==1?"alpha":"beta";w.reviewed=number==2;p.words={w};doc.replacePage(p);}
        const auto snapshot=doc.toJson();ui::ExperimentDialog dialog(doc);dialog.show();
        auto *references=dialog.findChild<QTableWidget *>("experimentReferences"),*pipelines=dialog.findChild<QTableWidget *>("experimentPipelines");QVERIFY(references && pipelines);references->selectRow(0);
        QTimer::singleShot(0,&dialog,[&]{auto *proof=qobject_cast<QDialog *>(QApplication::activeModalWidget());QVERIFY(proof);auto *text=proof->findChild<QPlainTextEdit *>("experimentReferenceText");QVERIFY(text);QCOMPARE(text->toPlainText(),"alpha");proof->accept();});dialog.findChild<QPushButton *>("proofreadExperimentReference")->click();
        QCOMPARE(doc.toJson(),snapshot);QVERIFY2(qobject_cast<QComboBox *>(references->cellWidget(0,2))->isEnabled(),qPrintable(dialog.findChild<QLabel *>("experimentStatus")->text()));qobject_cast<QComboBox *>(references->cellWidget(0,2))->setCurrentIndex(1);qobject_cast<QComboBox *>(references->cellWidget(1,2))->setCurrentIndex(2);
        pipelines->item(1,0)->setCheckState(Qt::Unchecked);qobject_cast<QLineEdit *>(pipelines->cellWidget(0,3))->setText("runtime-secret");
        dialog.findChild<QSpinBox *>("experimentRepetitions")->setValue(1);dialog.findChild<QSpinBox *>("experimentWarmups")->setValue(0);auto *run=dialog.findChild<QPushButton *>("runExperiments");run->click();QTRY_VERIFY_WITH_TIMEOUT(run->isEnabled(),10000);
        auto *results=dialog.findChild<QTableWidget *>("experimentResults");QCOMPARE(results->rowCount(),1);QCOMPARE(results->item(0,8)->text(),"Complete");results->selectRow(0);
        const QString output=dir.filePath("experiment.json");QPushButton *save=nullptr,*inspect=nullptr;for(auto *b:dialog.findChildren<QPushButton *>()){if(b->text()=="Save report…")save=b;if(b->text()=="Inspect readings…")inspect=b;}
        QVERIFY(save && inspect && save->isEnabled());QTimer::singleShot(0,&dialog,[&]{auto *file=qobject_cast<QFileDialog *>(QApplication::activeModalWidget());QVERIFY(file);file->selectFile(output);QMetaObject::invokeMethod(file,"accept",Qt::DirectConnection);});save->click();
        QFile report(output);QVERIFY(report.open(QIODevice::ReadOnly));const auto bytes=report.readAll();QVERIFY(!bytes.contains("runtime-secret"));QCOMPARE(QJsonDocument::fromJson(bytes).object()["schema"].toString(),"yagf-experiment/1");
        QTimer::singleShot(0,&dialog,[&]{auto *viewer=qobject_cast<QDialog *>(QApplication::activeModalWidget());QVERIFY(viewer);bool textFound=false;for(auto *text:viewer->findChildren<QPlainTextEdit *>())textFound|=text->toPlainText()=="alpha";QVERIFY(textFound);viewer->reject();});inspect->click();
        QPushButton *open=nullptr;for(auto *b:dialog.findChildren<QPushButton *>())if(b->text()=="Open report…")open=b;
        QVERIFY(open);QTimer::singleShot(0,&dialog,[&]{auto *file=qobject_cast<QFileDialog *>(QApplication::activeModalWidget());QVERIFY(file);file->selectFile(output);QMetaObject::invokeMethod(file,"accept",Qt::DirectConnection);});open->click();QCOMPARE(results->rowCount(),1);
        qputenv("PATH",oldPath);
    }
};
QTEST_MAIN(ExperimentDialogTests)
#include "experimentdialog_tests.moc"

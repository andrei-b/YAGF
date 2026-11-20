#include "symbolrulesdialog.h"
#include "symbolfixture.h"
#include <QComboBox>
#include <QDialogButtonBox>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QProgressBar>
#include <QPdfWriter>
#include <QPageSize>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>
#include <algorithm>
using namespace yagf;
class SymbolRulesDialogTests : public QObject {
    Q_OBJECT
    QTemporaryDir directory;
    core::OcrDocument book;
private slots:
    void initTestCase()
    {
        QVERIFY(directory.isValid());
        qputenv("XDG_DATA_HOME",QFile::encodeName(directory.path()));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,directory.path());
        QSettings::setPath(QSettings::NativeFormat,QSettings::UserScope,directory.path());
        book.sourcePath=directory.filePath("symbols.png"); book.engine="tesseract"; book.language="rus"; book.totalPages=1;
        QImage image(180,80,QImage::Format_RGB32); image.fill(Qt::white);
        { QPainter p(&image); p.drawImage(10,10,symbolFixture()); p.drawImage(65,10,symbolFixture(true,4)); p.drawImage(120,10,symbolFixture(false)); }
        QVERIFY(image.save(book.sourcePath));
        core::OcrPage page; page.number=1; page.width=page.sourceWidth=image.width(); page.height=page.sourceHeight=image.height(); page.engineId="tesseract"; page.language="rus";
        for (int i=0;i<3;++i) page.words.append({1,1,1,1,QRect(10+i*55,10,40,48),"ф","ф",45});
        page.rebuildHierarchy(); book.replacePage(page);
    }
    void scopeSeedSearchAndReview()
    {
        const auto before=book.toJson();
        ui::SymbolRulesDialog dialog(book,{},nullptr,QImage(book.sourcePath),QRect(10,10,40,48),1,"ф"); dialog.show();
        auto *scope=dialog.findChild<QComboBox *>("symbolRuleScope");
        auto *label=dialog.findChild<QLineEdit *>("symbolRuleLabel");
        auto *list=dialog.findChild<QListWidget *>("symbolTrainingExamples");
        QVERIFY(scope && label && list); label->setText("ψ"); scope->setCurrentIndex(1);
        QVERIFY(dialog.bookRules().isEmpty()); QCOMPARE(dialog.sharedRules().size(),1);
        scope->setCurrentIndex(0); QCOMPARE(dialog.bookRules().size(),1);
        dialog.findChild<QPushButton *>("saveSymbolCrop")->click(); QCOMPARE(list->count(),1);
        QCOMPARE(dialog.bookRules()[0].examples[0].decision,"positive");
        dialog.findChild<QPushButton *>("findSymbolExamples")->click();
        auto *find=dialog.findChild<QPushButton *>("findSymbolExamples"); QTRY_VERIFY_WITH_TIMEOUT(find->isEnabled(),10000);
        QVERIFY(list->count()>=3);
        const auto rule=dialog.bookRules()[0];
        for (int i=1;i<rule.examples.size();++i) QCOMPARE(rule.examples[i].decision,"pending");
        // Search proposals are never silently promoted into training labels.
        dialog.findChild<QPushButton *>("trainSymbolRule")->click();
        QVERIFY(!core::symbolRuleTrained(dialog.bookRules()[0]));
        QVERIFY(dialog.findChild<QLabel *>("symbolRuleMessage")->text().contains("at least"));
        list->setCurrentRow(1); dialog.findChild<QPushButton *>("symbolDecision-negative")->click();
        QCOMPARE(dialog.bookRules()[0].examples[1].decision,"negative");
        QCOMPARE(book.toJson(),before);
        dialog.reject(); QVERIFY(dialog.result()!=QDialog::Accepted);
    }
    void localTrainingApplyAndInvalidation()
    {
        auto snapshot=book; auto rule=symbolRuleFixture();
        for (auto &e:rule.examples) e.source=book.sourcePath;
        snapshot.symbolRules={rule};
        ui::SymbolRulesDialog dialog(snapshot,{}); dialog.show();
        dialog.findChild<QComboBox *>("symbolTrainingBackend")->setCurrentIndex(0);
        auto *train=dialog.findChild<QPushButton *>("trainSymbolRule"); train->click(); QTRY_VERIFY(train->isEnabled());
        QVERIFY(core::symbolRuleTrained(dialog.bookRules()[0]));
        bool sawConfirmation=false; QTimer confirmation;
        connect(&confirmation,&QTimer::timeout,&dialog,[&] {
            auto *message=qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (message && message->windowTitle()=="Apply symbol recognition") { sawConfirmation=true; message->button(QMessageBox::Yes)->click(); }
        }); confirmation.start(10);
        auto *apply=dialog.findChild<QPushButton *>("applySymbolRules"); apply->click(); QTRY_VERIFY_WITH_TIMEOUT(sawConfirmation,10000); confirmation.stop();
        QCOMPARE(dialog.changedPages().size(),1); QCOMPARE(dialog.changedPages()[0].words[0].text,"ψ");
        QCOMPARE(dialog.changedPages()[0].words[2].text,"ф"); QCOMPARE(book.page(1)->words[0].text,"ф");
        dialog.findChild<QLineEdit *>("symbolRuleLabel")->setText("☺");
        QVERIFY(!core::symbolRuleTrained(dialog.bookRules()[0])); QVERIFY(dialog.changedPages().isEmpty());
    }
    void searchCoversWholeRangeAndCanCancel()
    {
        auto snapshot=book; snapshot.pages.clear(); snapshot.totalPages=7; snapshot.sourcePath=directory.filePath("many-symbols.pdf");
        {
            QPdfWriter writer(snapshot.sourcePath); writer.setResolution(150);
            writer.setPageSize(QPageSize(QSizeF(288,288),QPageSize::Point)); writer.setPageMargins(QMarginsF(0,0,0,0));
            QPainter painter(&writer);
            for (int page=1;page<=7;++page) {
                if (page>1) QVERIFY(writer.newPage());
                for (int row=0;row<6;++row) for (int column=0;column<8;++column)
                    painter.drawImage(10+column*70,10+row*80,symbolFixture(column%3!=2));
            }
        }
        auto rule=symbolRuleFixture(); rule.examples.clear(); snapshot.symbolRules={rule};
        ui::SymbolRulesDialog dialog(snapshot,{}); dialog.show();
        auto *find=dialog.findChild<QPushButton *>("findSymbolExamples"); find->click();
        QTRY_VERIFY_WITH_TIMEOUT(find->isEnabled(),20000);
        const auto examples=dialog.bookRules()[0].examples;
        QCOMPARE(examples.size(),core::maximumSymbolExamples);
        QVERIFY(std::ranges::any_of(examples,[](const auto &e) { return e.page==7; }));
        QVERIFY(std::ranges::all_of(examples,[](const auto &e) { return e.decision=="pending"; }));
        QVERIFY(dialog.findChild<QLabel *>("symbolRuleMessage")->text().contains("across 7 pages"));
        ui::SymbolRulesDialog interrupted(snapshot,{}); interrupted.show();
        auto *search=interrupted.findChild<QPushButton *>("findSymbolExamples"); search->click();
        interrupted.findChild<QPushButton *>("cancelSymbolOperation")->click();
        QTRY_VERIFY_WITH_TIMEOUT(search->isEnabled(),10000);
        QVERIFY(interrupted.bookRules()[0].examples.isEmpty());
    }
    void remoteTrainingAndBadResponse()
    {
        auto snapshot=book; snapshot.symbolRules={symbolRuleFixture()};
        const auto expected=*core::trainSymbolRule(snapshot.symbolRules[0]);
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost,0)); bool sawRequest=false, invalid=false;
        connect(&server,&QTcpServer::newConnection,&server,[&] {
            auto *socket=server.nextPendingConnection(); auto buffer=std::make_shared<QByteArray>();
            connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
            connect(socket,&QTcpSocket::readyRead,socket,[&,socket,buffer] {
                *buffer+=socket->readAll(); const int end=buffer->indexOf("\r\n\r\n"); if (end<0) return;
                const auto header=buffer->left(end); const auto length=QRegularExpression("Content-Length: (\\d+)",QRegularExpression::CaseInsensitiveOption).match(QString::fromLatin1(header));
                if (!length.hasMatch() || buffer->size()<end+4+length.captured(1).toInt()) return;
                const auto request=QJsonDocument::fromJson(buffer->mid(end+4)).object();
                QVERIFY(header.contains("Bearer session-only")); QVERIFY(!buffer->contains("seed_png")); QVERIFY(!buffer->contains("image_png"));
                QCOMPARE(request["examples"].toArray().size(),3); sawRequest=true;
                auto model=expected; if (invalid) model["radius"]=.9;
                const auto body=QJsonDocument(QJsonObject{{"protocol","yagf-symbol-training/1"},{"model",model}}).toJson(QJsonDocument::Compact);
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "+QByteArray::number(body.size())+"\r\nConnection: close\r\n\r\n"+body); socket->disconnectFromHost();
            });
        });
        for (bool malformed:{false,true}) {
            invalid=malformed; sawRequest=false;
            ui::SymbolRulesDialog dialog(snapshot,{}); dialog.show();
            dialog.findChild<QComboBox *>("symbolTrainingBackend")->setCurrentIndex(1);
            dialog.findChild<QLineEdit *>("symbolTrainingUrl")->setText(QString("http://127.0.0.1:%1").arg(server.serverPort()));
            for (auto *line:dialog.findChildren<QLineEdit *>()) if (line->echoMode()==QLineEdit::Password) line->setText("session-only");
            auto *train=dialog.findChild<QPushButton *>("trainSymbolRule"); train->click(); QTRY_VERIFY_WITH_TIMEOUT(train->isEnabled(),10000);
            QVERIFY(sawRequest); QCOMPARE(core::symbolRuleTrained(dialog.bookRules()[0]),!malformed);
            QVERIFY(!QJsonDocument(core::symbolRulesJson(dialog.bookRules())).toJson().contains("session-only"));
        }
    }
};
QTEST_MAIN(SymbolRulesDialogTests)
#include "symbolrulesdialog_tests.moc"

#include "nativetrainingdialog.h"
#include "documentcommands.h"
#include "remoteocrengine.h"
#include "smartblocks.h"
#include "documentprocessor.h"
#include <QBuffer>
#include <QComboBox>
#include <QFile>
#include <QJsonDocument>
#include <QLabel>
#include <QListWidget>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QSettings>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QUndoStack>
#include <QTimer>
#include <QtTest>
using namespace yagf;
namespace {
const QString jid="00000000-0000-0000-0000-000000000001";
class TrainingServer:public QTcpServer {
public:
    QJsonObject submitted;
    bool keepTraining=false,pollInFlight=false,cancelled=false;
    TrainingServer() {if (!listen(QHostAddress::LocalHost)) qFatal("Could not listen");
        connect(this,&QTcpServer::newConnection,this,[this]{auto *socket=nextPendingConnection(); connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
            connect(socket,&QTcpSocket::readyRead,socket,[this,socket,bytes=QByteArray{}]()mutable {
                bytes+=socket->readAll(); const int split=bytes.indexOf("\r\n\r\n"); if (split<0 || socket->property("sent").toBool()) return;
                int length=0; for(const auto &line:bytes.left(split).split('\n')) if(line.toLower().startsWith("content-length:")) length=line.mid(15).trimmed().toInt();
                if(bytes.size()<split+4+length)return; socket->setProperty("sent",true);
                const auto path=bytes.split(' ').value(1); auto body=QJsonDocument::fromJson(bytes.mid(split+4,length)).object();
                QJsonObject result{{"protocol","yagf-native-training/1"}};
                if(path=="/v1/capabilities") result["profiles"]=QJsonArray{QJsonObject{{"id","tess"},{"label","Tesseract"},{"engine","tesseract"},{"ready",true}}};
                else if(path=="/v1/models") result["models"]=QJsonArray{};
                else if(path=="/v1/jobs") {submitted=body; result["id"]=jid; result["status"]="running";}
                else if(path==("/v1/jobs/"+jid+"/cancel").toUtf8()) {cancelled=true; result["id"]=jid; result["status"]="cancelled";}
                else if(path==("/v1/jobs/"+jid).toUtf8()) result=QJsonObject{{"protocol","yagf-native-training/1"},{"id",jid},{"profile_id","tess"},{"status","complete"},{"message","Validated"},{"eligible",true},{"model_url","/models/"+jid},{"baseline",QJsonObject{{"cer",.2}}},{"candidate",QJsonObject{{"cer",0.}}}};
                else if(path==("/models/"+jid+"/v1/recognize").toUtf8()) {
                    QImage image; image.loadFromData(QByteArray::fromBase64(body["image_png"].toString().toLatin1()),"PNG");
                    result={{"protocol","yagf-ocr/1"},{"engine_id","native"},{"width",image.width()},{"height",image.height()},
                        {"words",QJsonArray{QJsonObject{{"text","ψ"},{"confidence",.95},{"block",1},{"line",1},{"box",QJsonArray{1,1,12,12}}}}}};
                }
                const bool delayed=keepTraining && path==("/v1/jobs/"+jid).toUtf8();
                if (delayed) {pollInFlight=true; result["status"]="running"; result["eligible"]=false;}
                const auto payload=QJsonDocument(result).toJson(QJsonDocument::Compact);
                const auto reply=[socket,payload]{socket->write("HTTP/1.1 200 OK\r\nContent-Length: "+QByteArray::number(payload.size())+"\r\nConnection: close\r\n\r\n"+payload); socket->disconnectFromHost();};
                if (delayed) QTimer::singleShot(250,socket,reply); else reply();
            });
        });
    }
    QUrl url() const {return QUrl(QString("http://127.0.0.1:%1").arg(serverPort()));}
};
class FakeEngine:public processing::IOcrEngine {
public:
    QString name; int calls=0;
    explicit FakeEngine(QString name):name(name){}
    QString id()const override{return name;}
    processing::EngineCapabilities capabilities()const override{return {.supportsRegions=true,.supportsWordBoxes=true};}
    std::expected<yagf::core::RecognitionResult,processing::OcrError> recognizeResult(const QImage &image,const processing::OcrOptions &,std::stop_token)override {
        ++calls; core::OcrPage p; p.width=p.sourceWidth=image.width(); p.height=p.sourceHeight=image.height(); p.words.append({1,1,1,1,{5,5,60,18},"Native","Native",95}); p.rebuildHierarchy(); return yagf::processing::recognitionFromPage(p);
    }
};
}
class NativeTrainingTests:public QObject {
    Q_OBJECT
    QTemporaryDir directory;
    core::OcrDocument book;
private slots:
    void initTestCase() {
        qputenv("XDG_DATA_HOME",QFile::encodeName(directory.path()));
        QSettings::setDefaultFormat(QSettings::IniFormat); QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,directory.path()); QSettings::setPath(QSettings::NativeFormat,QSettings::UserScope,directory.path());
        book.sourcePath=directory.filePath("scan.png"); book.engine="tesseract"; book.language="rus";
        QImage image(400,180,QImage::Format_RGB32); image.fill(Qt::white); QPainter painter(&image); painter.setPen(Qt::black);
        core::OcrPage page; page.width=page.sourceWidth=image.width(); page.height=page.sourceHeight=image.height(); page.language="rus";
        for(int i=0;i<3;++i) {const QString text=QString("Строка номер %1").arg(i+1); QRect box(10,10+i*40,250,24); painter.drawText(box,text); page.words.append({1,1,1,i+1,box,text,text,95,i<2});}
        painter.end(); QVERIFY(image.save(book.sourcePath)); page.rebuildHierarchy(); book.replacePage(page);
    }
    void collectionReviewAndBundle() {
        std::atomic_bool cancelled=false; auto result=processing::collectNativeTrainingSamples(book,1,1,cancelled); QVERIFY2(result,qPrintable(result ? QString() : result.error()));
        QCOMPARE(result->samples.size(),3); QVERIFY(result->samples[0].previouslyReviewed); QCOMPARE(result->samples[0].decision,"pending"); QCOMPARE(result->samples[2].partition,"validation");
        QJsonArray items; for(auto &sample:result->samples) {QImage image; QVERIFY(image.loadFromData(sample.png,"PNG")); QCOMPARE(image.size(),sample.bounds.size()); sample.decision="approved"; items.append(processing::trainingSampleJson(sample));}
        auto restored=processing::readTrainingSamples(items); QVERIFY(restored); QCOMPARE(restored->size(),3);
        auto unicode=items; auto example=unicode[0].toObject(); example["text"]=QString::fromUtf8("ψ 🙂"); unicode[0]=example;
        QVERIFY(processing::readTrainingSamples(unicode));
        example["text"]="two\nlines"; unicode[0]=example; QVERIFY(!processing::readTrainingSamples(unicode));
        const auto folder=directory.filePath("bundle"); QVERIFY(QDir().mkpath(folder)); QVERIFY(processing::writeNativeTrainingBundle(folder,*result,cancelled));
        QFile train(folder+"/train.txt"); QVERIFY(train.open(QIODevice::ReadOnly)); QCOMPARE(train.readAll().count('\n'),2);
        QFile paddle(folder+"/paddle-validation.txt"); QVERIFY(paddle.open(QIODevice::ReadOnly)); QVERIFY(paddle.readAll().contains("\t"));
        QVERIFY(!processing::writeNativeTrainingBundle(folder,*result,cancelled));
        cancelled=true; QVERIFY(!processing::collectNativeTrainingSamples(book,1,1,cancelled));
    }
    void projectAndUndo() {
        auto doc=book; QUndoStack undo; const auto url="http://127.0.0.1:8772/models/"+jid;
        undo.push(new ui::ChangeNativeOcrModelCommand(doc,url)); QCOMPARE(doc.nativeOcrUrl,url); undo.undo(); QVERIFY(doc.nativeOcrUrl.isEmpty()); undo.redo();
        core::OcrDocument restored; QString error; QVERIFY2(core::OcrDocument::fromJson(doc.toJson(),&restored,&error),qPrintable(error)); QCOMPARE(restored.nativeOcrUrl,url);
        auto bad=doc.toJson(); bad["native_ocr_url"]="http://user:password@host/models/"+jid; QVERIFY(!core::OcrDocument::fromJson(bad,&restored,&error));
        processing::DocumentProcessor processor; processor.setEngine(std::make_unique<FakeEngine>("previous"));
        processor.setNativeEngine(std::make_unique<FakeEngine>("native-remote")); QCOMPARE(processor.engineId(),"native-remote"); processor.setNativeEngine(std::make_unique<FakeEngine>("another-native"));
        processor.setMathMode(true); processor.setNativeEngine({}); processor.setMathMode(false); QCOMPARE(processor.engineId(),"previous");
    }
    void remoteModelAndSharedReviewWorkflow() {
        TrainingServer server; QSettings().setValue("nativeTraining/url",server.url().toString());
        ui::NativeTrainingDialog dialog(book,1); dialog.show();
        auto *samples=dialog.findChild<QListWidget *>("nativeTrainingSamples"); auto *train=dialog.findChild<QPushButton *>("trainNativeModel"); auto *use=dialog.findChild<QPushButton *>("useNativeModel"); QVERIFY(samples && train && use);
        QTRY_COMPARE_WITH_TIMEOUT(samples->count(),3,4000); QVERIFY(!train->isEnabled()); QVERIFY(!use->isEnabled());
        // Loading the samples starts an asynchronous model-library refresh.
        // Wait until review controls are available before sending clicks.
        QTRY_VERIFY_WITH_TIMEOUT(dialog.findChild<QPushButton *>("approveNativeExample")->isEnabled(),3000);
        const auto reviewed=dialog.findChildren<QPushButton *>();
        for (auto *button:reviewed) if(button->text()=="Approve already-reviewed lines") button->click();
        samples->setCurrentRow(2); dialog.findChild<QPushButton *>("approveNativeExample")->click(); QTRY_VERIFY_WITH_TIMEOUT(train->isEnabled(),3000);
        train->click(); QTRY_VERIFY_WITH_TIMEOUT(use->isEnabled(),5000); QCOMPARE(server.submitted["samples"].toArray().size(),3);
        for (const auto &sample:server.submitted["samples"].toArray()) QCOMPARE(sample.toObject()["decision"].toString(),"approved");
        QVERIFY(book.nativeOcrUrl.isEmpty());
        train->click(); QTRY_VERIFY_WITH_TIMEOUT(use->isEnabled(),5000); QCOMPARE(server.submitted["base_model_id"].toString(),jid);
        auto *endpoint=dialog.findChild<QLineEdit *>("nativeTrainingEndpoint"); endpoint->setText("http://127.0.0.1:1");
        QVERIFY(!use->isEnabled()); QVERIFY(!train->isEnabled()); endpoint->setText(server.url().toString()); QVERIFY(use->isEnabled());
        use->click(); QCOMPARE(dialog.result(),int(QDialog::Accepted));
        QCOMPARE(dialog.selectedModelUrl().path(),"/models/"+jid);
        processing::RemoteOcrEngine engine(dialog.selectedModelUrl(),{},processing::RemoteOcrEngine::Provider::Native);
        QImage image(100,60,QImage::Format_RGB32); image.fill(Qt::white); auto result=engine.recognize(image,{.language="auto"},{});
        QVERIFY2(result,qPrintable(result ? QString() : result.error().message)); QCOMPARE(result->words[0].text,"ψ"); QCOMPARE(result->engineId,"native-remote");
    }
    void typedBlocksUseNativeTextProvider() {
        FakeEngine provider("native-remote"); QImage image(100,50,QImage::Format_RGB32); image.fill(Qt::white); QPainter p(&image); p.fillRect(QRect(5,5,60,18),Qt::black); p.end();
        auto result=processing::recognizeSmartBlocks(image,{.language="eng"},{{"text",image.rect(),{},false,"text"}},false,{},false,nullptr,nullptr,&provider);
        QVERIFY2(result,qPrintable(result ? QString() : result.error().message)); QVERIFY(provider.calls>0); QVERIFY(!result->words.isEmpty());
    }
    void cancelDuringStatusRequestCancelsTheTrainingJob() {
        TrainingServer server; server.keepTraining=true;
        QSettings().setValue("nativeTraining/url",server.url().toString());
        ui::NativeTrainingDialog dialog(book,1); dialog.show();
        auto *samples=dialog.findChild<QListWidget *>("nativeTrainingSamples");
        auto *train=dialog.findChild<QPushButton *>("trainNativeModel");
        QTRY_COMPARE_WITH_TIMEOUT(samples->count(),3,4000);
        auto *approve=dialog.findChild<QPushButton *>("approveNativeExample");
        QTRY_VERIFY_WITH_TIMEOUT(approve->isEnabled(),3000);
        for (int i=0;i<3;++i) {samples->setCurrentRow(i); approve->click();}
        QTRY_VERIFY_WITH_TIMEOUT(train->isEnabled(),3000); train->click();
        QTRY_VERIFY_WITH_TIMEOUT(server.pollInFlight,4000);
        for (auto *button:dialog.findChildren<QPushButton *>())
            if (button->text()=="Cancel operation") button->click();
        QTRY_VERIFY_WITH_TIMEOUT(server.cancelled,3000);
        QTRY_VERIFY_WITH_TIMEOUT(train->isEnabled(),3000);
        QVERIFY(!dialog.findChild<QPushButton *>("useNativeModel")->isEnabled());
    }
    void liveNativeWorker() {
        const QUrl url(qEnvironmentVariable("YAGF_NATIVE_TEST_MODEL_URL"));
        if (url.isEmpty()) QSKIP("Set YAGF_NATIVE_TEST_MODEL_URL and YAGF_NATIVE_TEST_IMAGE for a real host smoke test");
        QImage image(qEnvironmentVariable("YAGF_NATIVE_TEST_IMAGE")); QVERIFY(!image.isNull());
        processing::RemoteOcrEngine engine(url,{},processing::RemoteOcrEngine::Provider::Native);
        auto result=engine.recognize(image,{.language="auto"},{});
        QVERIFY2(result,qPrintable(result ? QString() : result.error().message));
        QCOMPARE(result->engineId,"native-remote"); QVERIFY(!result->words.isEmpty());
        QStringList text; for (const auto &word:result->words) text.append(word.text);
        const auto expected=qEnvironmentVariable("YAGF_NATIVE_TEST_EXPECTED");
        if (!expected.isEmpty()) QCOMPARE(text.join(' '),expected);
    }
};
QTEST_MAIN(NativeTrainingTests)
#include "nativetraining_tests.moc"

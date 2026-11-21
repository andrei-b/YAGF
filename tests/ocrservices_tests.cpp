#include "ocrservices.h"
#include "ocrservicesdialog.h"
#include "ocrreadingsdialog.h"
#include "documentprocessor.h"
#include "documentcommands.h"
#include "remoteocrengine.h"
#include "ocrreadings.h"
#include "tesseractengine.h"
#include "mathocrengine.h"
#include "aipagelayout.h"
#include <algorithm>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFile>
#include <QPainter>
#include <QJsonDocument>
#include <QLineEdit>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTabWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QUndoStack>
#include <QtTest>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
using namespace yagf;
namespace {
class VisionServer:public QTcpServer {
public:
    bool invalid=false,stall=false,compatible=true;
    int posts=0;
    QString prompt;
    QStringList prompts;
    QVector<QSize> crops;
    std::shared_ptr<std::atomic_bool> formulaRequested = std::make_shared<std::atomic_bool>(false);
    VisionServer() {
        if (!listen(QHostAddress::LocalHost)) qFatal("listen failed");
        connect(this,&QTcpServer::newConnection,this,[this] {
            auto *socket=nextPendingConnection(); connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
            connect(socket,&QTcpSocket::readyRead,socket,[this,socket,bytes=QByteArray{}]() mutable {
                bytes+=socket->readAll(); const int end=bytes.indexOf("\r\n\r\n");
                if (end<0 || socket->property("sent").toBool()) return;
                int length=0;
                for (const auto &line:bytes.left(end).split('\n')) if (line.toLower().startsWith("content-length:")) length=line.mid(15).trimmed().toInt();
                if (bytes.size()<end+4+length) return;
                socket->setProperty("sent",true);
                const auto path=bytes.split(' ').value(1); int status=200; QJsonObject result;
                if (path=="/api/v1/models") result={{"data",QJsonArray{QJsonObject{{"id",compatible ? "PaddleOCR-VL-1.6" : "text-only-model"}}}}};
                else if (path=="/api/v1/chat/completions") {
                    ++posts;
                    const auto body=QJsonDocument::fromJson(bytes.mid(end+4,length)).object();
                    const auto content=body["messages"].toArray().first().toObject()["content"].toArray();
                    prompt=content[1].toObject()["text"].toString();
                    prompts.append(prompt);
                    if (prompt == "Formula Recognition:") formulaRequested->store(true);
                    const auto data=content[0].toObject()["image_url"].toObject()["url"].toString().section(',',1).toLatin1();
                    QImage image; image.loadFromData(QByteArray::fromBase64(data),"PNG"); crops.append(image.size());
                    if (stall) return;
                    result={{"choices",QJsonArray{QJsonObject{{"finish_reason",invalid ? "length" : "stop"},
                        {"message",QJsonObject{{"content","\\[\\frac{333}{106}=3,141509\\ldots\\]"}}}}}}};
                } else status=404;
                const auto body=QJsonDocument(result).toJson(QJsonDocument::Compact);
                socket->write("HTTP/1.1 "+QByteArray::number(status)+" Result\r\nContent-Type: application/json\r\nContent-Length: "+QByteArray::number(body.size())+"\r\nConnection: close\r\n\r\n"+body);
                socket->disconnectFromHost();
            });
        });
    }
    QUrl url() const {return QUrl(QString("http://127.0.0.1:%1/api").arg(serverPort()));}
    processing::OcrService service() const {
        processing::OcrService s; s.provider="paddleocr-vl";s.label="Jetson formulas";s.url=url();s.model="PaddleOCR-VL-1.6";s.language="auto";return s;
    }
};

class TextWithFormula:public processing::IOcrEngine {
    int delay;
    std::shared_ptr<std::atomic_bool> formulaRequested;
public:
    explicit TextWithFormula(int delay=0, std::shared_ptr<std::atomic_bool> formulaRequested={})
        :delay(delay),formulaRequested(std::move(formulaRequested)){}
    QString id()const override{return "tesseract";}
    processing::EngineCapabilities capabilities()const override{return {.supportsRegions=true,.supportsWordBoxes=true};}
    std::expected<yagf::core::RecognitionResult,processing::OcrError> recognizeResult(const QImage &image,const processing::OcrOptions &options,std::stop_token stop)override {
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        while (formulaRequested && !formulaRequested->load() && !stop.stop_requested() && std::chrono::steady_clock::now()<deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (formulaRequested && !formulaRequested->load())
            return std::unexpected(processing::OcrError{"Formula service did not start alongside text OCR"});
        if (delay) QThread::msleep(delay);
        core::OcrPage page;page.number=options.pageNumber;page.width=page.sourceWidth=image.width();page.height=page.sourceHeight=image.height();page.language="eng";
        page.words={{options.pageNumber,1,1,1,{5,5,40,12},"Primary","Primary",95},
                    {options.pageNumber,2,1,1,{110,80,40,18},"ро","ро",95}};
        page.rebuildHierarchy();return yagf::processing::recognitionFromPage(page);
    }
};

class Server:public QTcpServer {
public:
    bool stall=false,layout=false,formulaOnly=false; int posts=0; QByteArray lastPath;
    QJsonObject healthOverride,recognitionOverride,lastRequest;
    Server() {if (!listen(QHostAddress::LocalHost)) qFatal("listen failed");
        connect(this,&QTcpServer::newConnection,this,[this]{auto *s=nextPendingConnection(); connect(s,&QTcpSocket::disconnected,s,&QObject::deleteLater);
            connect(s,&QTcpSocket::readyRead,s,[this,s,data=QByteArray{}]()mutable {
                data+=s->readAll(); const int end=data.indexOf("\r\n\r\n"); if(end<0 || s->property("sent").toBool()) return;
                int length=0; for (const auto &line:data.left(end).split('\n')) if(line.toLower().startsWith("content-length:")) length=line.mid(15).trimmed().toInt();
                if(data.size()<end+4+length)return; s->setProperty("sent",true); lastPath=data.split(' ').value(1);
                if(stall)return;
                const auto body=QJsonDocument::fromJson(data.mid(end+4,length)).object(); QJsonObject result;
                if(data.startsWith("POST "))lastRequest=body;
                if(data.startsWith("GET ")) result={{"protocol","yagf-ocr/1"},{"engine_id","native"},{"status","ready"},{"services",QJsonArray{
                    QJsonObject{{"path","/profiles/rus"},{"provider","native"},{"label","Russian model"},{"language","rus"},{"ready",true}},
                    QJsonObject{{"path","/gateway/doctr"},{"provider","doctr"},{"label","Remote docTR"},{"language","auto"},{"ready",true}},
                    QJsonObject{{"path","//other-host/"},{"provider","doctr"},{"label","Invalid"},{"ready",true}}}}};
                else {
                    ++posts; QImage image; image.loadFromData(QByteArray::fromBase64(body["image_png"].toString().toLatin1()),"PNG");
                    result={{"protocol","yagf-ocr/1"},{"engine_id","doctr"},{"width",image.width()},{"height",image.height()},
                        {"words",QJsonArray{QJsonObject{{"text","Other"},{"confidence",.95},{"box",QJsonArray{5,5,40,12}},{"block",1},{"line",1}}}}};
                }
                if (layout && data.startsWith("GET ")) result={{"protocol","yagf-layout/1"},{"status","ready"},{"model","PP-DocLayoutV3"}};
                if (formulaOnly && data.startsWith("GET ")) result={{"protocol","yagf-ocr/1"},{"engine_id","paddleocr"},{"status","ready"},{"model","PP-FormulaNet_plus-L"},{"tasks",QJsonArray{"formula"}}};
                if (layout && data.startsWith("POST ")) {
                    QImage image;image.loadFromData(QByteArray::fromBase64(body["image_png"].toString().toLatin1()),"PNG");
                    result={{"protocol","yagf-layout/1"},{"model","PP-DocLayoutV3"},{"width",image.width()},{"height",image.height()},
                        {"areas",QJsonArray{QJsonObject{{"id","formula"},{"label","display_formula"},{"type","math"},{"score",.95},{"box",QJsonArray{90,65,90,55}}},
                            QJsonObject{{"id","prose"},{"label","text"},{"type","text"},{"score",.95},{"box",QJsonArray{0,0,80,40}}}}}};
                }
                if(data.startsWith("GET ") && !healthOverride.isEmpty())result=healthOverride;
                if(data.startsWith("POST ") && !recognitionOverride.isEmpty())result=recognitionOverride;
                const auto bytes=QJsonDocument(result).toJson(QJsonDocument::Compact);
                s->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "+QByteArray::number(bytes.size())+"\r\nConnection: close\r\n\r\n"+bytes); s->disconnectFromHost();
            });
        });
    }
    QUrl url() const{return QUrl(QString("http://127.0.0.1:%1").arg(serverPort()));}
};
struct State {std::atomic_int started=0,finished=0; int required=1; bool stalled=false,failed=false; std::mutex mutex; QVector<QSize> images;};
class Engine:public processing::IOcrEngine {
    std::shared_ptr<State> state; QString label; bool companion;
public:
    Engine(std::shared_ptr<State> state,QString text,bool companion=false):state(std::move(state)),label(std::move(text)),companion(companion){}
    QString id()const override{return companion ? "test-companion" : "test-primary";}
    processing::EngineCapabilities capabilities()const override{return {};}
    std::expected<yagf::core::RecognitionResult,processing::OcrError> recognizeResult(const QImage &image,const processing::OcrOptions &options,std::stop_token stop)override {
        ++state->started; {std::lock_guard lock(state->mutex);state->images.append(image.size());}
        const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        while(state->started<state->required && !stop.stop_requested() && std::chrono::steady_clock::now()<until)std::this_thread::sleep_for(std::chrono::milliseconds(2));
        if(state->started<state->required) {++state->finished;return std::unexpected(processing::OcrError{"Engines were not started concurrently"});}
        while(companion && state->stalled && !stop.stop_requested())std::this_thread::sleep_for(std::chrono::milliseconds(2));
        ++state->finished;
        if(stop.stop_requested())return std::unexpected(processing::OcrError{{},true});
        if(companion && state->failed)return std::unexpected(processing::OcrError{"Offline test service"});
        core::OcrPage page; page.number=options.pageNumber;page.width=page.sourceWidth=image.width();page.height=page.sourceHeight=image.height();page.language="eng";
        page.words.append({options.pageNumber,1,1,1,{5,5,40,12},label,label,95});page.rebuildHierarchy(); return yagf::processing::recognitionFromPage(page);
    }
};
processing::OcrService service(const QString &label,const QString &path) {
    processing::OcrService s; s.provider="doctr";s.label=label;s.url=QUrl("http://127.0.0.1:8766/"+path);s.language="auto";s.enabled=true;return s;
}
}
class OcrServicesTests:public QObject {
    Q_OBJECT
    QTemporaryDir temporary;
    QImage image;
    QString scan;
private slots:
    void initTestCase() {
        QSettings::setDefaultFormat(QSettings::IniFormat); QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,temporary.path());
        QSettings::setPath(QSettings::NativeFormat,QSettings::UserScope,temporary.path());
        QVERIFY(QSettings("YAGF","YAGF Revival").fileName().startsWith(temporary.path()));
        qputenv("XDG_DATA_HOME",temporary.path().toUtf8());
        image=QImage(200,140,QImage::Format_RGB32);image.fill(Qt::white);QPainter painter(&image);QFont font;font.setPixelSize(11);painter.setFont(font);painter.drawText(QRect(5,5,40,12),"Primary");painter.end();scan=temporary.filePath("scan.png");QVERIFY(image.save(scan));
    }
    void liveFormulaAndLayoutServices() {
        const auto url=qEnvironmentVariable("YAGF_LIVE_FORMULA_URL");
        const auto layout=qEnvironmentVariable("YAGF_LIVE_LAYOUT_URL");
        const auto catalog=qEnvironmentVariable("YAGF_LIVE_CATALOG_URL");
        const auto source=qEnvironmentVariable("YAGF_LIVE_FORMULA_IMAGE");
        if (url.isEmpty() || source.isEmpty()) QSKIP("Set YAGF_LIVE_FORMULA_URL and YAGF_LIVE_FORMULA_IMAGE for the live formula/layout smoke test");
        QVector<QUrl> targets{QUrl(url)};if (!layout.isEmpty()) targets.append(QUrl(layout));
        if (!catalog.isEmpty()) targets.append(QUrl(catalog));
        auto discovered=processing::discoverOcrServices(targets,{},{});QVERIFY(discovered);
        auto formula=std::ranges::find_if(discovered->services,[](const auto &s){return s.ready && s.supportsFormulas();});
        QVERIFY(formula!=discovered->services.end());
        auto engine=processing::makeOcrServiceEngine(*formula);QVERIFY(engine);
        QImage image(source);QVERIFY(!image.isNull());
        auto reading=processing::recognizeFormulaBoxes(engine->get(),image,{}, {},{image.rect()});
        QVERIFY2(reading,qPrintable(reading ? QString() : reading.error().message));QVERIFY(!reading->words.isEmpty());
        qInfo().noquote()<<"Live formula reading:"<<reading->words.first().text;
        if (!layout.isEmpty()) {
            QVERIFY(std::ranges::any_of(discovered->services,[](const auto &s){return s.ready && s.supportsPageLayout();}));
            QImage page(qEnvironmentVariable("YAGF_LIVE_LAYOUT_IMAGE",source));QVERIFY(!page.isNull());
            auto areas=processing::detectPageAreas(page,{QUrl(layout)});QVERIFY2(areas,qPrintable(areas ? QString() : areas.error().message));
            qInfo()<<"Live layout areas:"<<areas->areas.size();QVERIFY(!areas->areas.isEmpty());
        }
        processing::OcrService local;local.provider="tesseract";local.label="Tesseract (this computer)";local.language="rus";
        ui::OcrServicesDialog dialog(discovered->services,local,nullptr,*formula);
        dialog.findChild<QLineEdit *>("ocrDiscoveryHosts")->setText(url+' '+layout+' '+catalog);
        dialog.show();QApplication::processEvents();
        QTRY_VERIFY_WITH_TIMEOUT(dialog.findChild<QPushButton *>("discoverOcrServices")->isEnabled(),5000);
        QCOMPARE(dialog.formulaService().key(),formula->key());
        auto *textSelector=dialog.findChild<QComboBox *>("textOcrEngine"),*formulaSelector=dialog.findChild<QComboBox *>("formulaOcrEngine");
        for (const auto &service:discovered->services) {
            QCOMPARE(textSelector->findData(service.key())>=0,service.supportsText());
            QCOMPARE(formulaSelector->findData(service.key())>=0,service.supportsFormulas());
            qInfo().noquote()<<service.label<<"text="<<service.supportsText()<<"formulas="<<service.supportsFormulas();
        }
        const auto screenshot=qEnvironmentVariable("YAGF_CAPTURE_RECOGNITION_SETTINGS");
        if (!screenshot.isEmpty()) QVERIFY(dialog.grab().save(screenshot));
        dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Cancel)->click();
    }
    void serviceSettingsKeepSecretsInMemoryAndNormalizeIdentity() {
        auto a=service("Other","nested");a.token="session-secret";
        const auto json=processing::ocrServicesJson({a});QVERIFY(!QJsonDocument(json).toJson().contains("session-secret"));
        auto restored=processing::readOcrServices(json);QCOMPARE(restored.size(),1);QVERIFY(restored[0].token.isEmpty());QCOMPARE(restored[0].key(),a.key());
        auto b=a;b.url.setPath("/nested/");QCOMPARE(a.key(),b.key());
        b.url=QUrl("http://user:password@host/");QVERIFY(!processing::makeOcrServiceEngine(b));QVERIFY(processing::readOcrServices(processing::ocrServicesJson({b})).isEmpty());
        QCOMPARE(processing::ocrDiscoveryTargets("host.example",{}).size(),8);QCOMPARE(processing::ocrDiscoveryTargets("https://host:9876/api",{}).size(),1);
    }
    void optionalModelTemplatesAreCompleteAndNeverEnableServices() {
        QSettings settings("YAGF","YAGF Revival");const auto keys=settings.allKeys();
        const auto models=processing::optionalOcrModels();QCOMPARE(models.size(),7);
        QSet<QString> names;
        for(auto service:models) {
            QVERIFY(service.profile.has_value());QVERIFY(!service.ready);QVERIFY(!service.enabled);QVERIFY(service.url.isEmpty());
            QVERIFY(!names.contains(service.model));names.insert(service.model);
            service.url=QUrl("http://model-host.example:8765");
            auto restored=processing::readOcrServices(processing::ocrServicesJson({service}));QCOMPARE(restored.size(),1);
            QVERIFY(processing::makeOcrServiceEngine(restored[0]));
            QCOMPARE(restored[0].profile->tasks,service.profile->tasks);
        }
        const auto &formula=models.last();QCOMPARE(formula.provider,"doctr");QCOMPARE(formula.model,"formulanet");
        QVERIFY(formula.supportsFormulas());QVERIFY(!formula.supportsText());
        QCOMPARE(settings.allKeys(),keys);
    }
    void optionalPresetIsAddedOnlyAfterConfirmationAndKeepsActiveChoices() {
        QSettings settings("YAGF","YAGF Revival");const auto keys=settings.allKeys();
        processing::OcrService primary;primary.provider="tesseract";primary.label="Local text";
        ui::OcrServicesDialog dialog({},primary);
        const auto initial=dialog.services().size();const auto formulaKey=dialog.formulaService().key();
        for(bool accept:{false,true}) {
            bool inspected=false;
            QTimer::singleShot(0,[&] {
                auto *modal=qobject_cast<QDialog *>(QApplication::activeModalWidget());
                if(!modal)return;
                auto *preset=modal->findChild<QComboBox *>("optionalOcrModelPreset");
                if(preset && preset->count()==8) {
                    preset->setCurrentIndex(7);
                    inspected=modal->findChild<QLineEdit *>("optionalOcrModelName")->text()=="formulanet";
                }
                if(accept && inspected)modal->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
                else modal->reject();
            });
            dialog.findChild<QPushButton *>("addOcrService")->click();QVERIFY(inspected);
            QCOMPARE(dialog.services().size(),initial+(accept?1:0));
            QCOMPARE(dialog.formulaService().key(),formulaKey);
            QCOMPARE(dialog.findChild<QComboBox *>("textOcrEngine")->currentData().toString(),primary.key());
        }
        QCOMPARE(dialog.services().last().profile->tasks,QStringList{"formula"});
        QVERIFY(!dialog.services().last().enabled);QCOMPARE(settings.allKeys(),keys);
    }
    void modelProfilesValidateAndKeepUnknownCapabilitiesUnknown() {
        QJsonObject json{{"model","document-model"},{"tasks",QJsonArray{"ocr","layout","table","formula"}},
            {"geometry","block"},{"formats",QJsonArray{"markdown","json"}},{"languages",QJsonArray{"auto","rus"}},
            {"max_image_pixels",12000000},{"supports_batching",true}};
        auto p=processing::readModelProfile(json);QVERIFY(p);QVERIFY(p->tasks.contains("text"));QVERIFY(!p->tasks.contains("ocr"));
        processing::ModelRequirements requirements;requirements.tasks={"text","table"};requirements.geometry="block";
        requirements.format="markdown";requirements.language="rus";requirements.imageSize={4000,3000};requirements.batching=true;
        QVERIFY(p->incompatibilities(requirements).isEmpty());
        auto copy=processing::readModelProfile(processing::modelProfileJson(*p));QVERIFY(copy);QVERIFY(copy->incompatibilities(requirements).isEmpty());
        requirements.geometry="word";QVERIFY(!p->incompatibilities(requirements).isEmpty());requirements.geometry="block";
        requirements.imageSize={4000,3001};QVERIFY(!p->incompatibilities(requirements).isEmpty());requirements.imageSize={4000,3000};
        requirements.language="deu";QVERIFY(!p->incompatibilities(requirements).isEmpty());requirements.language="rus";
        requirements.format="hocr";QVERIFY(!p->incompatibilities(requirements).isEmpty());
        for(const auto &key:QStringList{"tasks","formats","languages","geometry","max_image_pixels","supports_batching"}) {
            auto bad=json;bad[key]=QJsonObject{};QVERIFY(!processing::readModelProfile(bad));
        }
        for(double pixels:{-1.,0.,1.5,9007199254740992.}) {auto bad=json;bad["max_image_pixels"]=pixels;QVERIFY(!processing::readModelProfile(bad));}
        auto unknown=processing::readModelProfile(QJsonObject{{"model","legacy-shaped"},{"tasks",QJsonArray{"text"}}});QVERIFY(unknown);
        QVERIFY(!unknown->supportsBatching.has_value());QCOMPARE(unknown->geometry,"unknown");
        processing::ModelRequirements strict;strict.tasks={"text"};strict.batching=true;
        QVERIFY(!unknown->incompatibilities(strict).isEmpty());
    }
    void advertisedProfilesOverrideProviderAssumptionsAndDriveSelection() {
        processing::OcrService text;text.provider="surya";text.label="Text only";text.url=QUrl("http://model-host.example:8765");text.ready=true;
        processing::ModelProfile profile;profile.model="text-model";profile.tasks={"text"};profile.geometry="word";
        profile.formats={"json"};profile.languages={"auto"};profile.maxImagePixels=12000000;profile.supportsBatching=false;
        text.model=profile.model;text.profile=profile;
        QVERIFY(text.supportsText());QVERIFY(!text.supportsFormulas());
        auto formula=text;formula.model="formula-model";formula.profile->model=formula.model;
        formula.profile->tasks={"formula"};formula.profile->geometry="none";formula.token="session-secret";
        QVERIFY(formula.supportsFormulas());QVERIFY(!formula.supportsText());QVERIFY(text.key()!=formula.key());
        auto json=processing::ocrServicesJson({text,formula});QVERIFY(!QJsonDocument(json).toJson().contains("session-secret"));
        auto restored=processing::readOcrServices(json);QCOMPARE(restored.size(),2);QCOMPARE(restored[1].profile->geometry,"none");
        processing::ModelRequirements r;r.tasks={"formula"};r.imageSize={4000,3000};
        auto chosen=processing::selectOcrModel({text,formula},r,text.key());QVERIFY(chosen);QCOMPARE(chosen->key(),formula.key());
        formula.ready=false;QVERIFY(!processing::selectOcrModel({text,formula},r));formula.ready=true;
        auto other=formula;other.model="other";other.profile->model=other.model;
        QCOMPARE(processing::selectOcrModel({formula,other},r,other.key())->key(),other.key());
        ui::OcrServicesDialog dialog({text,formula},text,nullptr,formula);
        auto *textChoice=dialog.findChild<QComboBox *>("textOcrEngine");auto *formulaChoice=dialog.findChild<QComboBox *>("formulaOcrEngine");
        QVERIFY(textChoice->findData(text.key())>=0);QCOMPARE(textChoice->findData(formula.key()),-1);
        QCOMPARE(formulaChoice->findData(text.key()),-1);QCOMPARE(formulaChoice->currentData().toString(),formula.key());
        QVERIFY(dialog.findChild<QLabel *>("textOcrModelDetails")->text().contains("12000000"));
        QVERIFY(dialog.findChild<QLabel *>("formulaOcrModelDetails")->text().contains("none"));
    }
    void genericProfileDiscoveryNegotiatesTasksAndLimitsBeforeUpload() {
        Server server;processing::ModelProfile p;p.model="document-vlm";p.tasks={"text","table","formula"};
        p.geometry="none";p.formats={"markdown","json"};p.languages={"auto"};p.maxImagePixels=28000;p.supportsBatching=true;
        auto formula=p;formula.model="formula-specialist";formula.tasks={"formula"};
        server.healthOverride={{"protocol","yagf-ocr/1"},{"engine_id","example-vlm"},{"status","ready"},
            {"profiles",QJsonArray{processing::modelProfileJson(p),processing::modelProfileJson(formula)}}};
        auto discovered=processing::discoverOcrServices({server.url()},{},{});QVERIFY(discovered);QCOMPARE(discovered->services.size(),2);
        QCOMPARE(server.posts,0);QVERIFY(discovered->diagnostics.isEmpty());
        const auto service=discovered->services[0];QCOMPARE(service.model,p.model);QVERIFY(service.supportsText());QVERIFY(service.supportsFormulas());
        auto restored=processing::readOcrServices(processing::ocrServicesJson(discovered->services));QCOMPARE(restored.size(),2);
        QCOMPARE(restored[0].protocol,"yagf-ocr/1");
        auto engine=processing::makeOcrServiceEngine(service);QVERIFY(engine);QVERIFY((*engine)->capabilities().supportsTables);QVERIFY(!(*engine)->capabilities().supportsWordBoxes);
        processing::OcrOptions options;options.language="auto";
        auto result=(*engine)->recognizeResult(QImage(201,140,QImage::Format_RGB32),options,{});QVERIFY(!result);QCOMPARE(server.posts,0);
        options.language="rus";result=(*engine)->recognizeResult(image,options,{});QVERIFY(!result);QCOMPARE(server.posts,0);
        options.language="auto";options.task="layout";result=(*engine)->recognizeResult(image,options,{});QVERIFY(!result);QCOMPARE(server.posts,0);
        options.task="table";options.outputFormat="hocr";result=(*engine)->recognizeResult(image,options,{});QVERIFY(!result);QCOMPARE(server.posts,0);
        options.outputFormat="markdown";
        server.recognitionOverride={{"protocol","yagf-ocr/1"},{"engine_id","example-vlm"},{"width",image.width()},{"height",image.height()},
            {"markdown","| a | b |\n|---|---|\n| 1 | 2 |"}};
        result=(*engine)->recognizeResult(image,options,{});QVERIFY2(result,result?"":qPrintable(result.error().message));
        QCOMPARE(server.posts,1);QCOMPARE(server.lastRequest["model"].toString(),p.model);QCOMPARE(server.lastRequest["task"].toString(),"table");
        QCOMPARE(server.lastRequest["output_format"].toString(),"markdown");QVERIFY(result->text.isEmpty());QVERIFY(!result->markdown.isEmpty());
        QCOMPARE(result->provenance.model,p.model);QCOMPARE(result->provenance.task,"table");
        processing::DocumentProcessor processor(nullptr,nullptr);processor.setEngine(std::move(*engine));
        QCOMPARE(processor.primaryOcrService().key(),service.key());QVERIFY(processor.primaryOcrService().profile.has_value());
        server.healthOverride.remove("profiles");server.healthOverride["profile"]=QJsonObject{{"model","broken"},{"tasks",false}};
        discovered=processing::discoverOcrServices({server.url()},{},{});QVERIFY(discovered);QVERIFY(discovered->services.isEmpty());QVERIFY(!discovered->diagnostics.isEmpty());
        server.healthOverride={{"data",QJsonArray{QJsonObject{{"id",p.model},{"protocol","yagf-ocr/1"},{"engine_id","example-vlm"},{"profile",processing::modelProfileJson(p)}}}}};
        discovered=processing::discoverOcrServices({server.url()},{},{});QVERIFY(discovered);QCOMPARE(discovered->services.size(),1);
        QCOMPARE(discovered->services[0].provider,"example-vlm");QCOMPARE(discovered->services[0].model,p.model);
        QVERIFY(discovered->diagnostics.isEmpty());QCOMPARE(server.posts,1);
        auto gateway=service;gateway.url=QUrl(server.url().toString()+"/services/8768");
        server.healthOverride={{"protocol","yagf-ocr/1"},{"engine_id","native"},{"status","ready"},
            {"services",QJsonArray{QJsonObject{{"path","/services/8768"},{"provider","example-vlm"},{"label",p.model},
                {"model",p.model},{"ready",true},{"profile",processing::modelProfileJson(p)}}}}};
        discovered=processing::discoverOcrServices({server.url()},{gateway},{});QVERIFY(discovered);QCOMPARE(discovered->services.size(),1);
        QCOMPARE(discovered->services[0].key(),gateway.key());QCOMPARE(discovered->services[0].profile->formats,p.formats);
    }
    void directVisionDiscoveryAdvertisesFormulasWithoutUploadingImages() {
        VisionServer server;
        auto result=processing::discoverOcrServices({server.url()},{},{});
        QVERIFY(result);QCOMPARE(result->services.size(),1);QCOMPARE(server.posts,0);QVERIFY(result->diagnostics.isEmpty());
        const auto s=result->services.first();QCOMPARE(s.provider,"paddleocr-vl");QCOMPARE(s.model,"PaddleOCR-VL-1.6");QVERIFY(s.supportsFormulas());QVERIFY(s.supportsText());
        auto restored=processing::readOcrServices(processing::ocrServicesJson({s}));QCOMPARE(restored.size(),1);QCOMPARE(restored[0].tasks,s.tasks);QCOMPARE(restored[0].model,s.model);
        server.compatible=false;
        result=processing::discoverOcrServices({server.url()},{},{});QVERIFY(result);QVERIFY(result->services.isEmpty());
    }
    void layoutDiscoveryAndSelectionAreSeparateFromOcr() {
        Server server;server.layout=true;
        auto result=processing::discoverOcrServices({server.url()},{},{});QVERIFY(result);QCOMPARE(result->services.size(),1);
        auto layout=result->services[0];QVERIFY(layout.supportsPageLayout());QVERIFY(!layout.supportsText());QVERIFY(!layout.supportsFormulas());QCOMPARE(server.posts,0);
        layout.token="layout-session-token";
        processing::OcrService primary;primary.provider="tesseract";primary.label="Local text";
        ui::OcrServicesDialog dialog({layout},primary);
        auto *table=dialog.findChild<QTableWidget *>("ocrServicesTable");QVERIFY(table);
        int row=-1;for (int i=0;i<table->rowCount();++i) if (table->item(i,1)->text()==layout.label) row=i;
        QVERIFY(row>=0);table->setCurrentCell(row,1);
        QVERIFY(!dialog.findChild<QPushButton *>("usePrimaryOcrService")->isEnabled());
        QVERIFY(!(table->item(row,0)->flags()&Qt::ItemIsUserCheckable));
        dialog.findChild<QPushButton *>("useLayoutService")->click();
        QCOMPARE(dialog.pageLayoutUrl(),server.url().toString());QCOMPARE(dialog.pageLayoutToken(),layout.token);
        QCOMPARE(dialog.formulaService().provider,"rapid-latex-ocr");QVERIFY(!dialog.primaryService());
        dialog.findChild<QLineEdit *>("pageLayoutServiceUrl")->setText("file:///tmp/service");
        dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();QCOMPARE(dialog.result(),int(QDialog::Rejected));
        QVERIFY(dialog.pageLayoutToken().isEmpty());
    }
    void selectedFormulaEngineReceivesCropsAndUsesFormulaPrompt() {
        VisionServer server; auto engine=processing::makeOcrServiceEngine(server.service());QVERIFY(engine);
        const QRect box(50,40,80,30);
        auto page=processing::recognizeFormulaBoxes(engine->get(),image,{.language="rus",.pageNumber=7},{},{box});
        QVERIFY2(page,qPrintable(page ? QString() : page.error().message));
        QCOMPARE(server.prompt,"Formula Recognition:");QCOMPARE(server.crops,QVector<QSize>{box.size()});
        QCOMPARE(page->words.first().bounds,box);QCOMPARE(page->number,7);QCOMPARE(page->words.first().text,"\\frac{333}{106}=3,141509\\ldots");
        QCOMPARE(page->words.first().readingEngine,"paddleocr-vl-remote");QCOMPARE(page->regions.first().type,"equation");
        QVERIFY(page->words.first().geometryInferred);QVERIFY(page->words.first().sourceBoxes.isEmpty());
        QCOMPARE(page->recognitionEvidence.size(),1);QCOMPARE(page->recognitionEvidence[0].formulas.size(),1);
        QVERIFY(!page->recognitionEvidence[0].formulas[0].bounds);
        QCOMPARE(page->recognitionEvidence[0].provenance.imageSize,box.size());
        QCOMPARE(page->recognitionEvidence[0].provenance.inputArea,box);
        server.invalid=true;QStringList warnings;
        page=processing::recognizeFormulaBoxes(engine->get(),image,{}, {},{box},&warnings);
        QVERIFY(page);QVERIFY(page->words.isEmpty());QVERIFY(!warnings.isEmpty());
        server.invalid=false;server.stall=true;std::stop_source stop;QTimer::singleShot(80,this,[&]{stop.request_stop();});
        page=processing::recognizeFormulaBoxes(engine->get(),image,{},stop.get_token(),{box});
        QVERIFY(!page);QVERIFY(page.error().cancelled);
    }
    void visionTextRetainsMarkdownWithoutClaimingWordCoordinates() {
        VisionServer server; auto engine=processing::makeOcrServiceEngine(server.service());QVERIFY(engine);
        const auto result=(*engine)->recognizeResult(image,{.language="auto"},{});QVERIFY(result);
        QCOMPARE(result->text.size(),1);QVERIFY(!result->text[0].bounds);
        QCOMPARE(result->text[0].granularity,core::TextGranularity::Page);QVERIFY(!result->markdown.isEmpty());
        QVERIFY(result->structuredOutput.contains("choices"));QCOMPARE(result->provenance.model,"PaddleOCR-VL-1.6");
        const auto page=processing::recognitionToPage(*result);QVERIFY(page);QCOMPARE(page->words.size(),1);
        QVERIFY(page->words[0].geometryInferred);QVERIFY(page->words[0].sourceBoxes.isEmpty());
    }
    void formulaSelectionIsIndependentFromPrimaryAndCompanions() {
        VisionServer server;auto remote=server.service();remote.token="session-only";
        processing::OcrService primary;primary.provider="tesseract";primary.label="Local text";
        ui::OcrServicesDialog dialog({remote},primary,nullptr,remote);
        auto *selector=dialog.findChild<QComboBox *>("formulaOcrEngine");QVERIFY(selector);QCOMPARE(selector->count(),2);
        QCOMPARE(dialog.formulaService().key(),remote.key());QCOMPARE(dialog.formulaService().token,"session-only");QVERIFY(!dialog.primaryService());
        QVERIFY(std::ranges::none_of(dialog.services(),[](const auto &s){return s.enabled;}));
        selector->setCurrentIndex(0);QCOMPARE(dialog.formulaService().provider,"rapid-latex-ocr");
        QVERIFY(!QJsonDocument(processing::ocrServicesJson({remote})).toJson().contains("session-only"));
        processing::DocumentProcessor processor;QVERIFY(processor.setFormulaOcrService(remote));QCOMPARE(processor.engineId(),"tesseract");
        QCOMPARE(processor.formulaOcrService().key(),remote.key());QVERIFY(!processor.setFormulaOcrService(primary));
    }
    void settingsShowAllModelsWithSeparateTextAndFormulaChoices() {
        VisionServer server;auto vision=server.service();vision.model="PaddleOCR-VL-1.6";
        auto text1=service("Russian print","print"),text2=service("Russian handwriting","handwriting");
        text1.model="Print model";text2.model="Handwriting model";
        auto formula=service("Jetson formulas","formulas");formula.provider="paddleocr";formula.model="PP-FormulaNet_plus-L";formula.tasks={"formula"};formula.ready=true;
        auto layout=service("Page layout","layout");layout.provider="layout";
        text1.enabled=text2.enabled=formula.enabled=layout.enabled=false;
        processing::OcrService primary;primary.provider="tesseract";primary.label="Local text";primary.language="rus";
        ui::OcrServicesDialog dialog({text1,text2,formula,vision,layout},primary,nullptr,vision);
        auto *text=dialog.findChild<QComboBox *>("textOcrEngine"),*math=dialog.findChild<QComboBox *>("formulaOcrEngine");
        auto *tabs=dialog.findChild<QTabWidget *>("recognitionSettingsTabs");QVERIFY(tabs);QCOMPARE(tabs->currentIndex(),0);
        QCOMPARE(text->count(),4);QCOMPARE(math->count(),3);
        QVERIFY(text->findData(formula.key())<0);QVERIFY(text->findData(layout.key())<0);QVERIFY(math->findData(text1.key())<0);
        QVERIFY(math->itemText(math->findData(formula.key())).contains(formula.model));
        QVERIFY(text->itemText(text->findData(text1.key())).contains(text1.model));
        math->setCurrentIndex(math->findData(formula.key()));
        QCOMPARE(text->currentData().toString(),primary.key());QCOMPARE(dialog.formulaService().key(),formula.key());
        text->setCurrentIndex(text->findData(text2.key()));
        QCOMPARE(dialog.primaryService()->key(),text2.key());QCOMPARE(math->currentData().toString(),formula.key());
        QVERIFY(dialog.findChild<QLabel *>("formulaOcrModelDetails")->text().contains("Ready"));
        QVERIFY(std::ranges::none_of(dialog.services(),[](const auto &s){return s.enabled;}));
        dialog.findChild<QPushButton *>("manageOcrModels")->click();QCOMPARE(tabs->currentIndex(),2);
        text->setCurrentIndex(text->findData(primary.key()));
        auto *language=dialog.findChild<QLineEdit *>("textOcrLanguage");QCOMPARE(language->text(),"rus");
        language->setFocus();language->selectAll();QTest::keyClicks(language,"rus+eng");
        QCOMPARE(dialog.primaryService()->language,"rus+eng");QCOMPARE(dialog.formulaService().key(),formula.key());
    }
    void formulaOnlyDiscoveryRetainsModelAndOfflineSelection() {
        Server server;server.formulaOnly=true;
        auto discovered=processing::discoverOcrServices({server.url()},{},{});QVERIFY(discovered);QCOMPARE(discovered->services.size(),1);
        const auto formula=discovered->services.first();QCOMPARE(formula.model,"PP-FormulaNet_plus-L");QVERIFY(formula.supportsFormulas());QVERIFY(!formula.supportsText());
        auto restored=processing::readOcrServices(processing::ocrServicesJson({formula}));QCOMPARE(restored.size(),1);QCOMPARE(restored[0].model,formula.model);
        processing::OcrService primary;primary.provider="tesseract";primary.label="Local text";
        ui::OcrServicesDialog dialog(restored,primary,nullptr,restored[0]);
        dialog.findChild<QLineEdit *>("ocrDiscoveryHosts")->setText(server.url().toString());dialog.show();QApplication::processEvents();
        auto *refresh=dialog.findChild<QPushButton *>("discoverOcrServices");QTRY_VERIFY_WITH_TIMEOUT(refresh->isEnabled(),4000);
        QCOMPARE(dialog.formulaService().key(),formula.key());QVERIFY(dialog.formulaService().ready);QCOMPARE(server.posts,0);
        server.stall=true;refresh->click();QTRY_VERIFY_WITH_TIMEOUT(refresh->isEnabled(),4000);
        QCOMPARE(dialog.formulaService().key(),formula.key());QVERIFY(!dialog.formulaService().ready);
        QVERIFY(dialog.findChild<QLabel *>("formulaOcrModelDetails")->text().contains("Unavailable"));
        QCOMPARE(dialog.findChild<QComboBox *>("textOcrEngine")->currentData().toString(),primary.key());QCOMPARE(server.posts,0);
    }
    void mixedPageRoutesOnlyFormulasToTheSelectedEngine() {
        VisionServer server;core::OcrDocument book;book.sourcePath=scan;
        core::OcrPage stored;stored.number=1;stored.width=stored.sourceWidth=200;stored.height=stored.sourceHeight=140;stored.language="eng";
        stored.imageRegions={{"prose",{0,0,80,40},"eng",false,"text",true,"text",.95},
                             {"formula",{90,65,90,55},"eng",false,"math",true,"display_formula",.95}};
        book.replacePage(stored);
        QImage pixels=image;{QPainter painter(&pixels);painter.fillRect(QRect(110,80,40,18),Qt::black);}
        const auto path=temporary.filePath("mixed.png");QVERIFY(pixels.save(path));
        processing::DocumentProcessor processor(nullptr,&book);processor.setEngine(std::make_unique<TextWithFormula>());
        QVERIFY(processor.setFormulaOcrService(server.service()));
        QSignalSpy done(&processor,&processing::DocumentProcessor::recognized),failed(&processor,&processing::DocumentProcessor::failed);
        processor.recognize(path,"eng",1);QTRY_VERIFY_WITH_TIMEOUT(!done.isEmpty()||!failed.isEmpty(),5000);
        QVERIFY2(failed.isEmpty(),failed.isEmpty()?"":qPrintable(failed.first().first().toString()));
        const auto page=done.first().first().value<core::OcrPage>();QCOMPARE(page.words.size(),2);QCOMPARE(page.words[0].text,"Primary");
        QVERIFY(page.words[1].layoutBlock);QVERIFY(page.words[1].text.contains("333"));QVERIFY(server.posts>=1);
        for (const auto &crop:server.crops) QVERIFY(crop.width()<image.width() && crop.height()<image.height());
        QVERIFY(!page.words[1].sourceWordIds.isEmpty());
        done.clear();
        processor.recognizeTypedRegions(path,"eng",1,{stored.imageRegions[1]});
        QTRY_VERIFY_WITH_TIMEOUT(!done.isEmpty()||!failed.isEmpty(),5000);QVERIFY(failed.isEmpty());
        const auto selected=done.first().first().value<core::OcrPage>();
        QVERIFY(!selected.words.isEmpty());
        for (const auto &word:selected.words) QVERIFY(word.layoutBlock && word.readingEngine=="paddleocr-vl-remote");
        done.clear();processor.setMathMode(true);
        processor.recognize(path,"eng",1);
        QTRY_VERIFY_WITH_TIMEOUT(!done.isEmpty()||!failed.isEmpty(),5000);QVERIFY(failed.isEmpty());
        const auto mathOnly=done.first().first().value<core::OcrPage>();
        QVERIFY(!mathOnly.words.isEmpty());
        for (const auto &word:mathOnly.words) QVERIFY(word.layoutBlock && word.readingEngine=="paddleocr-vl-remote");
    }
    void automaticLayoutRefreshesOldBlocksAndPrioritizesFormulaRequests() {
        VisionServer vision;Server layout;layout.layout=true;
        core::OcrDocument book;book.sourcePath=scan;core::OcrPage stored;
        stored.number=1;stored.width=stored.sourceWidth=200;stored.height=stored.sourceHeight=140;stored.language="eng";
        stored.imageRegions={{"old-text",image.rect(),"eng",false,"text",true}};book.replacePage(stored);
        QImage pixels=image;{QPainter p(&pixels);p.fillRect(QRect(110,80,40,18),Qt::black);}
        const auto path=temporary.filePath("auto-layout.png");QVERIFY(pixels.save(path));
        processing::DocumentProcessor processor(nullptr,&book);processor.setEngine(std::make_unique<TextWithFormula>(80,vision.formulaRequested));
        QVERIFY(processor.setFormulaOcrService(vision.service()));QVERIFY(processor.setPageLayoutService({layout.url()},true));
        auto companion=processing::makeOcrServiceEngine(vision.service());QVERIFY(companion);
        std::vector<processing::ComparisonEngine> companions;companions.push_back({vision.service(),std::move(*companion)});
        processor.setComparisonEngines(std::move(companions));
        QSignalSpy done(&processor,&processing::DocumentProcessor::recognized),failed(&processor,&processing::DocumentProcessor::failed);
        processor.recognize(path,"eng",1);QTRY_VERIFY_WITH_TIMEOUT(!done.isEmpty()||!failed.isEmpty(),5000);
        QVERIFY2(failed.isEmpty(),failed.isEmpty()?"":qPrintable(failed.first().first().toString()));
        const auto page=done.first().first().value<core::OcrPage>();QCOMPARE(page.words.size(),2);
        QCOMPARE(page.words[0].text,"Primary");QVERIFY(page.words[1].layoutBlock);QVERIFY(page.words[1].text.contains("333"));
        QCOMPARE(vision.prompts,(QStringList{"Formula Recognition:","OCR:"}));QCOMPARE(page.ocrReadings.size(),1);
        QVERIFY(page.ocrReadings[0].error.isEmpty());QVERIFY(layout.posts>0);
        QVERIFY(std::ranges::any_of(page.imageRegions,[](const auto &r){return r.detectedLabel=="display_formula";}));
    }
    void sharedTextAndFormulaEndpointStaysSerialized_data() {
        QTest::addColumn<bool>("differentModel");
        QTest::addColumn<bool>("versionedUrl");
        QTest::newRow("same-model") << false << false;
        QTest::newRow("different-model-same-endpoint") << true << false;
        QTest::newRow("equivalent-versioned-url") << false << true;
    }
    void sharedTextAndFormulaEndpointStaysSerialized() {
        QFETCH(bool, differentModel);
        QFETCH(bool, versionedUrl);
        VisionServer vision;
        core::OcrDocument book; book.sourcePath=scan;
        core::OcrPage stored; stored.number=1; stored.width=stored.sourceWidth=200; stored.height=stored.sourceHeight=140;
        stored.imageRegions={{"text",{0,0,80,40},"auto",false,"text",false},
                             {"formula",{90,65,90,55},"auto",false,"math",false}};
        book.replacePage(stored);
        QImage pixels=image; {QPainter painter(&pixels);painter.fillRect(QRect(110,80,40,18),Qt::black);}
        const auto path=temporary.filePath("shared-service.png"); QVERIFY(pixels.save(path));
        auto primary=vision.service();
        if (differentModel) primary.model="PaddleOCR-VL-1.5";
        if (versionedUrl) primary.url.setPath(primary.url.path()+"/v1/");
        auto text=processing::makeOcrServiceEngine(primary); QVERIFY(text);
        processing::DocumentProcessor processor(nullptr,&book); processor.setEngine(std::move(*text));
        QVERIFY(processor.setFormulaOcrService(vision.service()));
        QSignalSpy done(&processor,&processing::DocumentProcessor::recognized),failed(&processor,&processing::DocumentProcessor::failed);
        processor.recognize(path,"auto",1);
        QTRY_VERIFY_WITH_TIMEOUT(!done.isEmpty()||!failed.isEmpty(),5000);
        QVERIFY2(failed.isEmpty(),failed.isEmpty()?"":qPrintable(failed.first().first().toString()));
        QVERIFY(vision.prompts.contains("Formula Recognition:"));
        QCOMPARE(vision.prompts.first(),"OCR:");
        QCOMPARE(vision.prompts.last(),"Formula Recognition:");
    }
    void discoveryIdentifiesCatalogChildrenWithoutSendingImages() {
        Server server;auto result=processing::discoverOcrServices({server.url()},{},{});QVERIFY(result);QCOMPARE(result->services.size(),2);QCOMPARE(server.posts,0);
        QCOMPARE(result->services[0].provider,"native");QCOMPARE(result->services[0].url.path(),"/profiles/rus");QCOMPARE(result->services[0].language,"rus");
        QCOMPARE(result->services[1].provider,"doctr");QCOMPARE(result->services[1].url.host(),server.url().host());
        auto remote=result->services[1];auto engine=processing::makeOcrServiceEngine(remote);QVERIFY(engine);
        auto page=(*engine)->recognize(image,{.language="auto"},{});QVERIFY2(page,qPrintable(page ? QString() : page.error().message));
        QCOMPARE(server.lastPath,"/gateway/doctr/v1/recognize");QCOMPARE(page->words.first().text,"Other");
    }
    void discoveryCancellationStopsUnresponsiveListeners() {
        Server server;server.stall=true;std::stop_source stop;QTimer::singleShot(100,this,[&]{stop.request_stop();});
        QElapsedTimer timer;timer.start();auto result=processing::discoverOcrServices({server.url()},{},stop.get_token());
        QVERIFY(!result);QVERIFY(result.error().cancelled);QVERIFY(timer.elapsed()<2000);
    }
    void primaryAndCompanionsStartConcurrentlyAndPersistSeparately() {
        auto state=std::make_shared<State>();state->required=3;
        core::OcrDocument book;book.sourcePath=scan;processing::DocumentProcessor processor(nullptr,&book);
        processor.setEngine(std::make_unique<Engine>(state,"Primary"));
        std::vector<processing::ComparisonEngine> companions;
        companions.push_back({service("Engine A","a"),std::make_unique<Engine>(state,"Alternative A",true)});
        companions.push_back({service("Engine B","b"),std::make_unique<Engine>(state,"Alternative B",true)});processor.setComparisonEngines(std::move(companions));
        QSignalSpy done(&processor,&processing::DocumentProcessor::recognized),failed(&processor,&processing::DocumentProcessor::failed);
        processor.recognize(scan,"eng",1);QTRY_VERIFY_WITH_TIMEOUT(!done.isEmpty()||!failed.isEmpty(),5000);QVERIFY2(failed.isEmpty(),failed.isEmpty()?"":qPrintable(failed.first().first().toString()));
        const auto page=done.first().first().value<core::OcrPage>();QCOMPARE(page.words[0].text,"Primary");QCOMPARE(page.ocrReadings.size(),2);QCOMPARE(state->started.load(),3);
        QCOMPARE(core::matchedOcrReading(page,{0},page.ocrReadings[0]),"Alternative A");book.replacePage(page);
        core::OcrDocument restored;QString error;QVERIFY2(core::OcrDocument::fromJson(book.toJson(),&restored,&error),qPrintable(error));QCOMPARE(restored.pages[0].ocrReadings.size(),2);
        QVERIFY(!restored.pages[0].ocrReadings[0].recognitionEvidence.isEmpty());
        QCOMPARE(core::recognitionResultJson(restored.pages[0].ocrReadings[0].recognitionEvidence[0]),
                 core::recognitionResultJson(page.ocrReadings[0].recognitionEvidence[0]));
        auto edited=core::acceptOcrReading(page,{0},page.ocrReadings[0],"Alternative A");QVERIFY(edited);QCOMPARE(edited->words[0].originalText,"Primary");
        QUndoStack undo;undo.push(new ui::ReplacePageCommand(book,1,*edited,"Use OCR reading"));QVERIFY(book.plainText().contains("Alternative A"));undo.undo();QVERIFY(book.plainText().contains("Primary"));undo.redo();
        auto malformed=book.toJson();auto pages=malformed["pages"].toArray();auto entry=pages[0].toObject();auto readings=entry["ocr_readings"].toArray();auto r=readings[0].toObject();r["area"]=QJsonArray{0,0,10000,10000};readings[0]=r;entry["ocr_readings"]=readings;pages[0]=entry;malformed["pages"]=pages;QVERIFY(!core::OcrDocument::fromJson(malformed,&restored,&error));
    }
    void failedCompanionDoesNotFailOrChangeThePrimary() {
        auto state=std::make_shared<State>();state->required=2;state->failed=true;processing::DocumentProcessor processor;
        processor.setEngine(std::make_unique<Engine>(state,"Primary"));std::vector<processing::ComparisonEngine> companions;
        companions.push_back({service("Offline","offline"),std::make_unique<Engine>(state,"Other",true)});processor.setComparisonEngines(std::move(companions));
        QSignalSpy done(&processor,&processing::DocumentProcessor::recognized),failed(&processor,&processing::DocumentProcessor::failed),warnings(&processor,&processing::DocumentProcessor::specialistWarning);
        processor.recognize(scan,"eng",1);QTRY_VERIFY_WITH_TIMEOUT(!done.isEmpty()||!failed.isEmpty(),4000);QVERIFY(failed.isEmpty());
        const auto page=done.first().first().value<core::OcrPage>();QCOMPARE(page.words[0].text,"Primary");QCOMPARE(page.ocrReadings.size(),1);QVERIFY(page.ocrReadings[0].error.contains("Offline"));QVERIFY(!warnings.isEmpty());
    }
    void cancellationJoinsAllCompanionWorkers() {
        auto state=std::make_shared<State>();state->required=2;state->stalled=true;processing::DocumentProcessor processor;
        processor.setEngine(std::make_unique<Engine>(state,"Primary"));std::vector<processing::ComparisonEngine> companions;
        companions.push_back({service("Slow","slow"),std::make_unique<Engine>(state,"Other",true)});processor.setComparisonEngines(std::move(companions));
        QSignalSpy cancelled(&processor,&processing::DocumentProcessor::cancelled),done(&processor,&processing::DocumentProcessor::recognized);
        processor.recognize(scan,"eng",1);QTRY_COMPARE_WITH_TIMEOUT(state->started.load(),2,2000);processor.cancel();QTRY_VERIFY_WITH_TIMEOUT(!cancelled.isEmpty(),2000);
        QVERIFY(done.isEmpty());QCOMPARE(state->finished.load(),2);QVERIFY(!processor.isBusy());
    }
    void selectedBlockReadingsRetainCropCoordinates() {
        auto state=std::make_shared<State>();std::vector<processing::ComparisonEngine> engines;engines.push_back({service("Other","other"),std::make_unique<Engine>(state,"Other",true)});
        processing::ParallelOcrRun run(engines,image,{.language="eng"},{{10,10,70,30},{100,80,60,30}},{});auto readings=run.finish();QCOMPARE(readings.size(),1);QCOMPARE(readings[0].items.size(),2);
        QCOMPARE(readings[0].items[0].bounds.topLeft(),QPoint(15,15));QCOMPARE(readings[0].items[1].bounds.topLeft(),QPoint(105,85));
        auto crop=core::cropOcrReadings(readings,{10,10,70,30});QCOMPARE(crop[0].items.size(),1);QCOMPARE(crop[0].items[0].bounds.topLeft(),QPoint(5,5));
        core::translateOcrReadings(crop,{10,10});core::mergeOcrReadings(readings,crop,{10,10,70,30});QCOMPARE(readings[0].items.size(),2);
    }
    void discoveryDialogSelectsPrimaryAndCompanions() {
        Server server;QSettings("YAGF","YAGF Revival").setValue("ocr/discoveryHosts",server.url().toString());
        processing::OcrService primary;primary.provider="tesseract";primary.label="Local Tesseract";
        ui::OcrServicesDialog dialog({},primary);dialog.show();auto *table=dialog.findChild<QTableWidget *>("ocrServicesTable");QVERIFY(table);
        QTRY_COMPARE_WITH_TIMEOUT(table->rowCount(),3,4000);QTRY_VERIFY_WITH_TIMEOUT(dialog.findChild<QPushButton *>("discoverOcrServices")->isEnabled(),4000);
        int russian=-1,doctr=-1;for(int r=0;r<table->rowCount();++r){if(table->item(r,1)->text()=="Russian model")russian=r;if(table->item(r,1)->text()=="Remote docTR")doctr=r;}
        QVERIFY(russian>=0&&doctr>=0);table->item(doctr,0)->setCheckState(Qt::Checked);table->setCurrentCell(russian,1);dialog.findChild<QPushButton *>("usePrimaryOcrService")->click();
        if (qEnvironmentVariableIsSet("YAGF_CAPTURE_OCR_UI")) {QApplication::processEvents();dialog.grab().save("/tmp/yagf-ocr-services.png");}
        QVERIFY(dialog.primaryService());QCOMPARE(dialog.primaryService()->url.path(),"/profiles/rus");
        dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();QCOMPARE(dialog.result(),int(QDialog::Accepted));
        QCOMPARE(std::ranges::count_if(dialog.services(),[](const auto &s){return s.enabled;}),1);
    }
    void reviewDialogUsesOnlyAnExplicitlySelectedReading() {
        core::OcrPage page;page.width=page.sourceWidth=image.width();page.height=page.sourceHeight=image.height();page.language="eng";
        page.words.append({1,1,1,1,{5,5,40,12},"Primary","Primary",95});page.rebuildHierarchy();
        core::OcrReading reading;reading.source="fixture";reading.engine="doctr-remote";reading.label="Other engine";reading.language="eng";reading.area=image.rect();reading.items.append({{5,5,40,12},"Other",1,1,1,false});page.ocrReadings.append(reading);
        ui::OcrReadingsDialog dialog(page,image);dialog.show();auto *use=dialog.findChild<QPushButton *>("useOcrReading");QVERIFY(use&&use->isEnabled());QVERIFY(!dialog.hasChanges());
        if (qEnvironmentVariableIsSet("YAGF_CAPTURE_OCR_UI")) {QApplication::processEvents();dialog.grab().save("/tmp/yagf-ocr-readings.png");}
        use->click();QVERIFY(dialog.hasChanges());QCOMPARE(dialog.editedPage().words[0].text,"Other");QCOMPARE(page.words[0].text,"Primary");QVERIFY(dialog.editedPage().words[0].reviewed);
        auto larger=reading;larger.items[0].bounds={0,0,200,120};larger.items[0].text="A large paragraph\nwith several lines";QVERIFY(core::matchedOcrReading(page,{0},larger).isEmpty());
    }
    void liveDiscoveredEngines() {
        const auto base=QUrl(qEnvironmentVariable("YAGF_LIVE_OCR_URL"));
        if (base.isEmpty()) QSKIP("Set YAGF_LIVE_OCR_URL and YAGF_LIVE_OCR_IMAGE for the real Jetson smoke test");
        auto found=processing::discoverOcrServices({base},{},{});QVERIFY(found);
        core::OcrDocument book;book.sourcePath=qEnvironmentVariable("YAGF_LIVE_OCR_IMAGE");book.useRawOcrOutput=true;QVERIFY(QFile::exists(book.sourcePath));
        processing::DocumentProcessor processor(nullptr,&book);processor.setEngine(std::make_unique<processing::TesseractEngine>());
        std::vector<processing::ComparisonEngine> companions;
        for (auto s:found->services) if (s.ready && (s.url.path()=="/profiles/tesseract-eng" || s.url.path()=="/profiles/kraken-print" || s.provider=="paddleocr")) {
            auto engine=processing::makeOcrServiceEngine(s);QVERIFY(engine);companions.push_back({s,std::move(*engine)});
        }
        QCOMPARE(companions.size(),size_t(3));processor.setComparisonEngines(std::move(companions));
        QSignalSpy done(&processor,&processing::DocumentProcessor::recognized),failed(&processor,&processing::DocumentProcessor::failed);
        processor.recognize(book.sourcePath,"eng",1);QTRY_VERIFY_WITH_TIMEOUT(!done.isEmpty()||!failed.isEmpty(),35000);QVERIFY(failed.isEmpty());
        const auto page=done.first().first().value<core::OcrPage>();QVERIFY(!page.words.isEmpty());QCOMPARE(page.ocrReadings.size(),3);
        for (const auto &r:page.ocrReadings) {QVERIFY2(r.error.isEmpty(),qPrintable(r.label+": "+r.error));QVERIFY(!r.items.isEmpty());qInfo().noquote()<<r.label<<core::ocrReadingText(r);}
    }
};
QTEST_MAIN(OcrServicesTests)
#include "ocrservices_tests.moc"

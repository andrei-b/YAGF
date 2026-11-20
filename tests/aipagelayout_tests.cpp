#include "aipagelayoutdialog.h"
#include "documentcommands.h"
#include "exportpage.h"
#include "latexdocumentexporter.h"
#include "scientificbookexporter.h"
#include <QComboBox>
#include <QFile>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QListWidget>
#include <QCheckBox>
#include <QGraphicsView>
#include <QGraphicsPixmapItem>
#include <QToolButton>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QUndoStack>
#include <QtTest>
using namespace yagf;
namespace {
QJsonObject area(QString id, QString type, QRect box, double score=.9, QString label="text") {
    return {{"id",id},{"type",type},{"label",label},{"score",score},
            {"box",QJsonArray{box.x(),box.y(),box.width(),box.height()}}};
}
class Server : public QTcpServer {
public:
    QJsonArray areas{area("a","text",QRect(80,0,100,100))};
    std::function<void(QJsonObject &)> change;
    QByteArray headers;
    bool hold=false;
    int requests=0;
    Server() {
        if (!listen(QHostAddress::LocalHost)) qFatal("Could not listen");
        connect(this,&QTcpServer::newConnection,this,[this]{
            auto *socket=nextPendingConnection();
            connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
            connect(socket,&QTcpSocket::readyRead,socket,[this,socket,bytes=QByteArray{}]() mutable {
                bytes+=socket->readAll(); int split=bytes.indexOf("\r\n\r\n");
                if (split<0 || socket->property("sent").toBool()) return;
                int size=0;
                for (const auto &line:bytes.left(split).split('\n')) if (line.toLower().startsWith("content-length:")) size=line.mid(15).trimmed().toInt();
                if (bytes.size()<split+4+size) return;
                socket->setProperty("sent",true); headers=bytes.left(split); ++requests;
                auto request=QJsonDocument::fromJson(bytes.mid(split+4,size)).object();
                QImage image; image.loadFromData(QByteArray::fromBase64(request["image_png"].toString().toLatin1()),"PNG");
                if (hold) return;
                QJsonObject result{{"protocol","yagf-layout/1"},{"width",image.width()},{"height",image.height()},
                                   {"model","fixture-layout"},{"areas",areas}};
                if (change) change(result);
                const auto payload=QJsonDocument(result).toJson(QJsonDocument::Compact);
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "+QByteArray::number(payload.size())+"\r\nConnection: close\r\n\r\n"+payload);
                socket->disconnectFromHost();
            });
        });
    }
    QUrl url() const { return QUrl(QString("http://127.0.0.1:%1").arg(serverPort())); }
};
QImage sample() { QImage image(200,200,QImage::Format_RGB32); image.fill(Qt::white); image.setPixelColor(10,140,Qt::blue); return image; }
}
class PageLayoutTests : public QObject {
    Q_OBJECT
    QTemporaryDir directory;
private slots:
    void initTestCase() {
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,directory.path());
        QSettings::setPath(QSettings::NativeFormat,QSettings::UserScope,directory.path());
    }
    void clientGeometryAndValidation() {
        Server server; processing::PageLayoutOptions options{server.url(),"test-token",.5};
        server.areas.append(area("inline","mixed",QRect(90,20,15,15),.8,"inline_formula"));
        auto result=processing::detectPageAreas(sample(),options); QVERIFY(result); QCOMPARE(result->areas.size(),1);
        QCOMPARE(result->areas[0].bounds,QRect(80,0,100,100)); QCOMPARE(result->areas[0].type,"mixed");
        QVERIFY(server.headers.contains("POST /v1/layout ")); QVERIFY(server.headers.contains("Authorization: Bearer test-token"));
        for (int mode=0;mode<6;++mode) {
            server.change=[mode](QJsonObject &root){
                if (mode==0) root["width"]=201;
                if (mode==1) root["protocol"]="wrong";
                auto areas=root["areas"].toArray(); auto a=areas[0].toObject();
                if (mode==2) a["box"]=QJsonArray{180,0,100,100};
                if (mode==3) a["score"]="0.8";
                if (mode==4) a["box"]=QJsonArray{80.5,0,100,100};
                areas[0]=a; if (mode==5) areas.append(a); root["areas"]=areas;
            };
            QVERIFY(!processing::detectPageAreas(sample(),options));
        }
        for (const auto &bad:QJsonArray{QJsonObject{{"label","image"},{"type","picture"},{"score",.95}},
                                       QJsonObject{{"label","text"},{"type","text"},{"score",.8}},
                                       QJsonObject{{"label","image"},{"type","picture"},{"score","0.8"}}}) {
            server.change=[bad](QJsonObject &root) { auto areas=root["areas"].toArray(); auto a=areas[0].toObject(); a["alternatives"]=QJsonArray{bad}; areas[0]=a; root["areas"]=areas; };
            QVERIFY(!processing::detectPageAreas(sample(),options));
        }
        server.change={}; server.hold=true; std::stop_source stop;
        QTimer::singleShot(50,this,[&]{stop.request_stop();});
        auto cancelled=processing::detectPageAreas(sample(),options,stop.get_token()); QVERIFY(!cancelled && cancelled.error().cancelled);
        options.timeoutMs=50; QVERIFY(!processing::detectPageAreas(sample(),options));
    }
    void manualBlocksAndUndo() {
        core::ImageRegion manual{"manual",QRect(0,0,30,30),"rus",false,"text",false};
        core::ImageRegion oldAuto{"old",QRect(0,100,50,50),{},false,"text",true};
        QVector<processing::PageArea> areas{{"image","picture",QRect(10,10,80,80),.99},{"chart","picture",QRect(80,80,80,80),.9}};
        auto merged=processing::mergePageAreas({manual,oldAuto},areas,true); QCOMPARE(merged.size(),2);
        QCOMPARE(merged[0].id,"manual"); QCOMPARE(merged[0].language,"rus"); QCOMPARE(merged[1].content,"picture"); QVERIFY(merged[1].notText);
        QCOMPARE(merged[1].detectedLabel,"chart"); QCOMPARE(merged[1].detectionScore,.9);
        auto appended=processing::mergePageAreas({manual,oldAuto},areas,false); QCOMPARE(appended.size(),3);
        core::OcrDocument book; book.sourcePath=directory.filePath("scan.png"); core::OcrPage page; page.width=page.height=page.sourceWidth=page.sourceHeight=200; page.imageRegions={manual,oldAuto}; book.replacePage(page);
        auto after=page; after.imageRegions=merged;
        QUndoStack undo; undo.push(new ui::ReplacePageCommand(book,1,after,"AI page areas")); QCOMPARE(book.page(1)->imageRegions[1].content,"picture");
        undo.undo(); QCOMPARE(book.page(1)->imageRegions[1].id,"old"); undo.redo();
        core::OcrDocument restored; QString error; QVERIFY2(core::OcrDocument::fromJson(book.toJson(),&restored,&error),qPrintable(error));
        QCOMPARE(restored.page(1)->imageRegions[1].detectedLabel,"chart"); QCOMPARE(restored.page(1)->imageRegions[1].detectionScore,.9);
        auto json=book.toJson(); auto pages=json["pages"].toArray(); auto p=pages[0].toObject(); auto regions=p["image_regions"].toArray(); auto r=regions[1].toObject(); r["detection_score"]=2.; regions[1]=r; p["image_regions"]=regions; pages[0]=p; json["pages"]=pages;
        QVERIFY(!core::OcrDocument::fromJson(json,&restored,&error));
    }
    void reviewBeforeApply() {
        Server server;
        auto text=area("text","text",QRect(80,0,100,100));
        text["alternatives"]=QJsonArray{QJsonObject{{"label","image"},{"type","picture"},{"score",.8}}};
        server.areas={area("manual-overlap","picture",QRect(0,0,70,70),.99,"image"),text,
                      area("overlap","picture",QRect(90,20,50,50),.7,"image"),area("chart","picture",QRect(0,130,100,40),.8,"chart")};
        QSettings().setValue("layout/url",server.url().toString());
        core::ImageRegion manual{"manual",QRect(0,0,20,20),"rus",false,"text",false};
        core::ImageRegion automatic{"old",QRect(0,120,100,40),{},false,"text",true};
        ui::AiPageLayoutDialog dialog(sample(),{manual,automatic},nullptr,38); dialog.show();
        auto *apply=dialog.findChild<QPushButton *>("applyPageAreas"); auto *list=dialog.findChild<QListWidget *>("layoutAreas");
        auto *preview=dialog.findChild<QGraphicsView *>("layoutPreview");
        QVERIFY(apply && list && preview); QVERIFY(!apply->isEnabled()); QCOMPARE(list->count(),2);
        QCOMPARE(preview->sceneRect(),QRectF(sample().rect()));
        bool hasPage=false; for (auto *item:preview->scene()->items()) if (auto *pixmap=qgraphicsitem_cast<QGraphicsPixmapItem *>(item)) hasPage=!pixmap->pixmap().isNull(); QVERIFY(hasPage);
        // No Analyze click: opening the window starts exactly one request.
        QTRY_VERIFY_WITH_TIMEOUT(apply->isEnabled(),3000); QCOMPARE(server.requests,1); QCOMPARE(list->count(),5);
        auto *use0=dialog.findChild<QCheckBox *>("layoutUseArea0");
        auto *use1=dialog.findChild<QCheckBox *>("layoutUseArea1");
        auto *use2=dialog.findChild<QCheckBox *>("layoutUseArea2");
        QVERIFY(use0 && use1 && use2); QVERIFY(!use0->isEnabled()); QVERIFY(use1->isChecked()); QVERIFY(!use2->isChecked());
        use2->click(); QVERIFY(!use1->isChecked()); use1->click(); QVERIFY(!use2->isChecked());
        auto *type=dialog.findChild<QComboBox *>("layoutType1"); QVERIFY(type);
        auto *alternative=type->parentWidget()->findChild<QToolButton *>("layoutAlternative"); QVERIFY(alternative && !alternative->isHidden());
        QVERIFY(alternative->text().contains("Picture") && alternative->text().contains("80%"));
        alternative->click(); QCOMPARE(type->currentData().toString(),"picture");
        auto regions=dialog.regions(); QCOMPARE(regions.size(),3); QCOMPARE(regions[0].id,"manual"); QCOMPARE(regions[1].content,"picture");
        QVERIFY(regions[1].notText); QCOMPARE(regions[1].detectedLabel,"image"); QCOMPARE(regions[1].detectionScore,.8);
        alternative->click(); QCOMPARE(type->currentData().toString(),"text");
        alternative->click(); QCOMPARE(type->currentData().toString(),"picture");
        auto *thumb=type->parentWidget()->findChild<QToolButton *>("layoutThumbnail"); QVERIFY(thumb && !thumb->icon().isNull());
        dialog.findChild<QToolButton *>("layoutFitPage")->click(); const auto wholePage=preview->transform().m11(); thumb->click(); QVERIFY(preview->transform().m11()>wholePage);
        QCOMPARE(automatic.id,"old"); QCOMPARE(manual.language,"rus");
        apply->click(); QCOMPARE(dialog.result(),int(QDialog::Accepted));
    }
    void failedAnalysisCannotApply() {
        Server server; server.change=[](QJsonObject &root){root["width"]=500;}; QSettings().setValue("layout/url",server.url().toString());
        ui::AiPageLayoutDialog dialog(sample(),{}); dialog.show(); auto *analyze=dialog.findChild<QPushButton *>("analyzePageAreas"); analyze->click();
        QTRY_VERIFY_WITH_TIMEOUT(analyze->isEnabled(),3000); QVERIFY(!dialog.findChild<QPushButton *>("applyPageAreas")->isEnabled()); QVERIFY(dialog.regions().isEmpty());
        QVERIFY(dialog.findChild<QLabel *>("layoutMessage")->text().contains("incompatible"));
    }
    void picturesSurviveHierarchyAndExports() {
        core::OcrDocument book; book.sourcePath=directory.filePath("scan.png"); QVERIFY(sample().save(book.sourcePath));
        core::OcrPage page; page.width=page.height=page.sourceWidth=page.sourceHeight=200;
        page.words.append({1,1,1,1,QRect(80,10,70,20),"Text","Text",90});
        page.imageRegions.append({"picture",QRect(0,130,100,40),{},true,"picture",true,"chart",.95}); page.rebuildHierarchy();
        QCOMPARE(page.regions.size(),1); auto regions=processing::exportRegions(page); QCOMPARE(regions.size(),2); QVERIFY(regions[1].isFigure()); QVERIFY(regions[1].paragraphs.isEmpty());
        book.replacePage(page); std::atomic_bool cancelled=false;
        const auto html=directory.filePath("pictures.html"); auto result=processing::ScientificBookExporter::write(book,html,cancelled); QVERIFY2(result.status==processing::ExportResult::Status::Success,qPrintable(result.error));
        QFile file(html); QVERIFY(file.open(QIODevice::ReadOnly)); const auto data=file.readAll();
        auto start=data.indexOf("data:image/png;base64,"); QVERIFY(start>=0); start+=22;
        const auto crop=QImage::fromData(QByteArray::fromBase64(data.mid(start,data.indexOf('"',start)-start)));
        QCOMPARE(crop.size(),QSize(100,40)); QCOMPARE(crop.pixelColor(10,10),QColor(Qt::blue));
        const auto latex=directory.filePath("pictures.tex"); result=processing::LatexDocumentExporter::write(book,latex,cancelled); QVERIFY2(result.status==processing::ExportResult::Status::Success,qPrintable(result.error));
        QFile tex(latex); QVERIFY(tex.open(QIODevice::ReadOnly)); const auto source=tex.readAll(); QVERIFY(source.contains("\\includegraphics")); QVERIFY(source.contains("Text"));
        page.regions.append(regions[1]); QCOMPARE(processing::exportRegions(page).size(),2);
        page.words.clear(); page.rebuildHierarchy(); book.replacePage(page);
        result=processing::ScientificBookExporter::write(book,directory.filePath("picture-only.html"),cancelled);
        QVERIFY2(result.status==processing::ExportResult::Status::Success,qPrintable(result.error));
    }
    void liveJetson() {
        const auto url=qEnvironmentVariable("YAGF_LAYOUT_LIVE_URL");
        if (url.isEmpty()) QSKIP("Set YAGF_LAYOUT_LIVE_URL and YAGF_LAYOUT_LIVE_IMAGE for the installed worker smoke test.");
        const QImage image(qEnvironmentVariable("YAGF_LAYOUT_LIVE_IMAGE")); QVERIFY(!image.isNull());
        auto result=processing::detectPageAreas(image,{QUrl(url)}); QVERIFY2(result,qPrintable(result ? QString() : result.error().message));
        QCOMPARE(result->model,"PP-DocLayoutV3");
        QVERIFY(std::ranges::any_of(result->areas,[](const auto &area){return area.type=="picture";}));
        QVERIFY(std::ranges::any_of(result->areas,[](const auto &area){return area.type=="text";}));
        QSettings().setValue("layout/url",url);
        ui::AiPageLayoutDialog dialog(image,{}); dialog.show();
        auto *apply=dialog.findChild<QPushButton *>("applyPageAreas");
        QTRY_VERIFY_WITH_TIMEOUT(apply->isEnabled(),120000);
        QVERIFY(std::ranges::any_of(dialog.regions(),[](const auto &r){return r.content=="picture" && r.notText;}));
        QVERIFY(dialog.grab().save("/tmp/yagf-ai-layout-dialog.png"));
    }
};
QTEST_MAIN(PageLayoutTests)
#include "aipagelayout_tests.moc"

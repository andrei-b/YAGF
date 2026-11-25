#include "aipagelayoutdialog.h"
#include "textguidedanalysis.h"
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
#include <QPainter>
#include <QSettings>
#include <QListWidget>
#include <QCheckBox>
#include <QGraphicsView>
#include <QGraphicsPixmapItem>
#include <QGraphicsRectItem>
#include <QToolButton>
#include <QSlider>
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
    QVector<double> thresholds;
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
                thresholds.append(request["threshold"].toDouble());
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
    void historyKeepsEveryLayoutAnalysisAndOriginalCoordinates() {
        Server server;QSettings("YAGF","YAGF Revival").setValue("layout/url",server.url().toString());
        ui::AiPageLayoutDialog dialog(sample(),{});
        processing::OcrOptions context;context.historyCoordinates.sourceSize={400,300};
        context=processing::croppedOcrOptions(context,{40,20,sample().width(),sample().height()});
        dialog.enableHistory(context,{},core::preprocessingEvidence({},150));dialog.show();
        QTRY_COMPARE_WITH_TIMEOUT(server.requests,1,2000);
        QTRY_VERIFY_WITH_TIMEOUT(dialog.findChild<QPushButton *>("analyzePageAreas")->isEnabled(),2000);
        auto history=dialog.recordedHistory();QCOMPARE(history.runs.size(),1);QVERIFY(history.runs[0].output.contains("areas"));
        QCOMPARE(history.runs[0].processedCrop.topLeft(),QPoint(40,20));QCOMPARE(history.runs[0].role,"layout");
        QVERIFY(core::validateRecognitionHistory(history));
        dialog.findChild<QPushButton *>("analyzePageAreas")->click();
        QTRY_VERIFY_WITH_TIMEOUT(dialog.findChild<QPushButton *>("analyzePageAreas")->isEnabled(),2000);
        QCOMPARE(dialog.recordedHistory().runs.size(),2);dialog.done(QDialog::Rejected);
        QCOMPARE(dialog.recordedHistory().runs.size(),2);
    }
    void legacyLayoutSettingsRemainReadable() {
        QSettings current("YAGF","YAGF Revival");current.remove("layout/url");
        QSettings().setValue("layout/url","http://127.0.0.1:9871");
        ui::AiPageLayoutDialog dialog(sample(),{});
        QCOMPARE(dialog.findChild<QLineEdit *>("layoutEndpoint")->text(),"http://127.0.0.1:9871");
        QSettings().remove("layout/url");
    }
    void clientGeometryAndValidation() {
        Server server; processing::PageLayoutOptions options{server.url(),"test-token",.5};
        server.areas.append(area("inline","mixed",QRect(90,20,15,15),.8,"inline_formula"));
        auto result=processing::detectPageAreas(sample(),options); QVERIFY(result); QCOMPARE(result->areas.size(),1);
        QCOMPARE(result->rawAreas.size(),2);QCOMPARE(result->rawAreas[0].type,"text");QCOMPARE(result->rawAreas[1].id,"inline");
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
    void blockSplitRequiresAtLeastTwoReviewedSubdivisions() {
        Server server;server.areas={area("one","text",QRect(10,10,60,30))};
        QSettings("YAGF","YAGF Revival").setValue("layout/url",server.url().toString());
        ui::AiPageLayoutDialog dialog(sample(),{},nullptr,1,{},true);dialog.show();
        auto *apply=dialog.findChild<QPushButton *>("applyPageAreas");QVERIFY(apply);QCOMPARE(apply->text(),"Apply split");
        QTRY_VERIFY_WITH_TIMEOUT(dialog.findChild<QPushButton *>("analyzePageAreas")->isEnabled() && server.requests==1,3000);
        QVERIFY(!apply->isEnabled());QCOMPARE(dialog.regions().size(),1);
        server.areas.append(area("two","math",QRect(90,70,70,30)));
        dialog.findChild<QPushButton *>("analyzePageAreas")->click();QTRY_VERIFY_WITH_TIMEOUT(apply->isEnabled(),3000);
        QCOMPARE(dialog.regions().size(),2);dialog.close();
    }
    void displayFormulaTouchingParagraphPadding() {
        QImage page(200, 120, QImage::Format_RGB32);
        page.fill(Qt::white);
        const QRect paragraph(6, 6, 180, 44), formula(45, 49, 102, 20);
        page.setPixelColor(formula.center(), Qt::black);
        Server server;
        server.areas = {area("paragraph", "text", paragraph, .91),
                        area("equation", "math", formula, .89, "display_formula")};
        processing::PageLayoutOptions options{server.url()};
        const auto layout = processing::detectPageAreas(page, options);
        QVERIFY(layout);
        QCOMPARE(layout->rawAreas.last().bounds, formula);
        const auto merged = processing::mergePageAreas({}, layout->areas, true);
        QCOMPARE(merged.size(), 2);
        QCOMPARE(merged.first().bounds, paragraph);
        QCOMPARE(merged.last().bounds, formula);

        QSettings settings("YAGF", "YAGF Revival");
        settings.setValue("layout/url", server.url().toString());
        settings.setValue("layout/threshold", .5);
        ui::AiPageLayoutDialog dialog(page, {});
        dialog.show();
        QTRY_COMPARE_WITH_TIMEOUT(dialog.regions().size(), 2, 3000);
        QCOMPARE(dialog.regions().last().bounds, formula);
        auto *slider = dialog.findChild<QSlider *>();
        QVERIFY(slider);
        slider->setValue(90);
        QCOMPARE(dialog.regions().size(), 1);
        slider->setValue(50);
        QCOMPARE(dialog.regions().size(), 2);

        auto proposals = layout->rawAreas;
        page.setPixelColor(formula.topLeft(), Qt::black);
        const core::RegionInk ink(page);
        QVERIFY(ink.conflicts(paragraph,formula));
        QCOMPARE(processing::mergePageAreas({}, proposals, true).size(), 2);
        proposals.first().bounds.translate(0, 10);
        page.fill(Qt::white);
        QVERIFY(core::RegionInk(page).conflicts(proposals.first().bounds,formula));
        QCOMPARE(processing::mergePageAreas({}, proposals, true).size(), 2);
    }
    void overlappingLineProposalsCanBothBeApplied() {
        QImage page(180,100,QImage::Format_RGB32); page.fill(Qt::white);
        QPainter painter(&page);
        const QRect descender(48,16,5,32), ascender(92,33,6,31);
        painter.fillRect(descender,Qt::black); painter.fillRect(ascender,Qt::black); painter.end();
        Server server;
        server.areas = {area("upper","text",{10,12,150,36},.94),
                        area("lower","text",{10,33,150,36},.83)};
        QSettings settings("YAGF","YAGF Revival");
        settings.setValue("layout/url",server.url().toString());
        settings.setValue("layout/threshold",.5);
        ui::AiPageLayoutDialog dialog(page,{}); dialog.show();
        QTRY_COMPARE_WITH_TIMEOUT(dialog.regions().size(),2,3000);
        const auto regions=dialog.regions();
        QVERIFY(regions[0].bounds.intersects(regions[1].bounds));
        QVERIFY(!regions[0].shape().intersects(regions[1].shape()));
        QVERIFY(QRegion(descender).subtracted(regions[0].shape()).isEmpty());
        QVERIFY(QRegion(ascender).subtracted(regions[1].shape()).isEmpty());
        auto *score=dialog.findChild<QSlider *>(); QVERIFY(score);
        score->setValue(90); QCOMPARE(dialog.regions().size(),1);
        QVERIFY(dialog.regions()[0].parts.isEmpty());
        score->setValue(50); QCOMPARE(dialog.regions().size(),2);
        QCOMPARE(dialog.regions()[1].shape(),regions[1].shape());
        QCOMPARE(server.requests,1);
    }
    void overlappingParagraphsKeepBothBlocksWhenGlyphMarginsTouch() {
        QImage page(180,130,QImage::Format_RGB32); page.fill(Qt::white);
        const QRect upperGlyph(65,46,9,15), lowerGlyph(75,61,8,16);
        QPainter painter(&page);
        painter.fillRect(upperGlyph,Qt::black);
        painter.fillRect(lowerGlyph,Qt::black);
        painter.end();
        Server server;
        server.areas = {area("upper","mixed",{10,8,160,54},.94),
                        area("lower","mixed",{10,58,160,62},.95)};
        const auto layout = processing::detectPageAreas(page, {server.url()});
        QVERIFY(layout);
        const auto merged = processing::mergePageAreas({},layout->areas,true);
        QCOMPARE(merged.size(),2);
        QVERIFY(!merged[0].shape().intersects(merged[1].shape()));
        QVERIFY(QRegion(upperGlyph).subtracted(merged[0].shape()).isEmpty());
        QVERIFY(QRegion(lowerGlyph).subtracted(merged[1].shape()).isEmpty());

        QSettings settings("YAGF", "YAGF Revival");
        settings.setValue("layout/url",server.url().toString());
        settings.setValue("layout/threshold",.5);
        ui::AiPageLayoutDialog dialog(page,{});
        dialog.show();
        QTRY_COMPARE_WITH_TIMEOUT(dialog.regions().size(),2,3000);
        QCOMPARE(dialog.regions()[0].shape(),merged[0].shape());
        QCOMPARE(dialog.regions()[1].shape(),merged[1].shape());
    }
    void inlineIntegralWithDetachedLimitsSurvivesParagraphConsolidation() {
        QImage gray(210,130,QImage::Format_Grayscale8);
        gray.fill(Qt::white);
        QPainter p(&gray);
        for (auto box : {QRect(8,8,155,10),QRect(7,64,50,10),QRect(80,57,6,3),
                         QRect(80,59,2,29),QRect(77,87,5,3),QRect(80,45,6,7),
                         QRect(77,94,10,7),QRect(95,66,34,9),QRect(149,64,52,10)})
            p.fillRect(box, Qt::black);
        p.end();
        const auto modelAreas = QVector<processing::PageArea>{
            {"text","text",{4,5,200,100},.94,{},"text","paragraph"},
            {"inline_formula","mixed",{73,44,60,58},.85,{},"mixed","integral"}};
        auto proposals = modelAreas;
        processing::consolidateInlineAreas(proposals, gray);
        const auto math = std::ranges::find(proposals,QString("math"),&processing::PageArea::type);
        QVERIFY(math != proposals.end());
        QVERIFY(math->bounds.contains(QRect(77,45,10,56)));
        QCOMPARE(math->id,QString("integral"));
        QCOMPARE(math->score,.85);
        const core::ImageRegion kept{"kept",{4,0,200,12},{},false,"text"};
        const auto merged = processing::mergePageAreas({kept},proposals,true);
        QVERIFY(std::ranges::any_of(merged,[&](const auto &r){return r.content=="math" && r.bounds==math->bounds;}));
        QPainter erase(&gray); erase.fillRect(77,94,10,7,Qt::white); erase.end();
        proposals = modelAreas;
        processing::consolidateInlineAreas(proposals,gray);
        QCOMPARE(proposals.size(),1);
        QCOMPARE(proposals[0].type,QString("mixed"));
    }

    void recognitionGuidesLayoutDeskewAndPreservesWordGeometry() {
        QImage scan(360, 260, QImage::Format_RGB32);
        scan.fill(Qt::white);
        core::OcrPage page;
        page.width = page.sourceWidth = scan.width(); page.height = page.sourceHeight = scan.height();
        page.number = 1;
        QPainter painter(&scan);
        for (int line = 0; line < 4; ++line) {
            for (int i = 0; i < 5; ++i) {
                QRect box(18 + i*60, 20 + line*42 + i*3, 32, 12);
                page.words.append({1, 1, 1, line+1, box, "word", "word", 95});
                painter.fillRect(box, Qt::black);
            }
        }
        painter.end();
        page.rebuildHierarchy();
        page.words.first().reviewed = true;
        const auto id = page.words.first().id;
        page.imageRegions.append({"manual", {14, 18, 280, 36}, {}, false, "text"});
        auto prepared = processing::prepareLayoutPage(scan, scan, page, 0, "eng");
        QVERIFY2(prepared, qPrintable(prepared ? QString() : prepared.error()));
        QVERIFY(std::abs(prepared->page.preprocessing.deskewDegrees - 2.86) < .5);
        QCOMPARE(prepared->page.coordinates.outputSize(), QSizeF(prepared->image.size()));
        QCOMPARE(prepared->page.words.first().id, id);
        QVERIFY(prepared->page.words.first().reviewed);
        QCOMPARE(prepared->page.imageRegions.first().id, QString("manual"));
        auto again = processing::prepareLayoutPage(scan, prepared->image, prepared->page, 0, "eng");
        QVERIFY(again);
        QCOMPARE(again->page.preprocessing.deskewDegrees, prepared->page.preprocessing.deskewDegrees);
        for (auto &word : page.words) word.geometryInferred = true;
        QVERIFY(!processing::recognizedTextGuidance(page.words).skewDegrees);
        for (auto &word : page.words) { word.geometryInferred = false; word.layoutBlock = true; }
        QVERIFY(!processing::recognizedTextGuidance(page.words).skewDegrees);
    }
    void manualBlocksAndUndo() {
        core::ImageRegion manual{"manual",QRect(0,0,30,30),"rus",false,"text",false};
        core::ImageRegion oldAuto{"old",QRect(0,100,50,50),{},false,"text",true};
        QVector<processing::PageArea> areas{{"image","picture",QRect(10,10,80,80),.99},{"chart","picture",QRect(80,80,80,80),.9}};
        auto merged=processing::mergePageAreas({manual,oldAuto},areas,true); QCOMPARE(merged.size(),3);
        QCOMPARE(merged[0].id,"manual"); QCOMPARE(merged[0].language,"rus"); QCOMPARE(merged[1].content,"picture"); QVERIFY(merged[1].notText);
        QVERIFY(merged[0].shape().intersects(merged[1].shape()));
        QCOMPARE(merged[2].detectedLabel,"chart"); QCOMPARE(merged[2].detectionScore,.9);
        auto appended=processing::mergePageAreas({manual,oldAuto},areas,false); QCOMPARE(appended.size(),4);
        core::OcrDocument book; book.sourcePath=directory.filePath("scan.png"); core::OcrPage page; page.width=page.height=page.sourceWidth=page.sourceHeight=200; page.imageRegions={manual,oldAuto}; book.replacePage(page);
        auto after=page; after.imageRegions=merged;
        QUndoStack undo; undo.push(new ui::ReplacePageCommand(book,1,after,"AI page areas")); QCOMPARE(book.page(1)->imageRegions[1].content,"picture");
        undo.undo(); QCOMPARE(book.page(1)->imageRegions[1].id,"old"); undo.redo();
        core::OcrDocument restored; QString error; QVERIFY2(core::OcrDocument::fromJson(book.toJson(),&restored,&error),qPrintable(error));
        const auto &saved=restored.page(1)->imageRegions;
        QCOMPARE(saved.size(),3); QVERIFY(saved[0].shape().intersects(saved[1].shape()));
        QCOMPARE(saved[2].detectedLabel,"chart"); QCOMPARE(saved[2].detectionScore,.9);
        auto json=book.toJson(); auto pages=json["pages"].toArray(); auto p=pages[0].toObject(); auto regions=p["image_regions"].toArray(); auto r=regions[1].toObject(); r["detection_score"]=2.; regions[1]=r; p["image_regions"]=regions; pages[0]=p; json["pages"]=pages;
        QVERIFY(!core::OcrDocument::fromJson(json,&restored,&error));
    }
    void reviewBeforeApply() {
        Server server;
        auto text=area("text","text",QRect(80,0,100,100));
        text["alternatives"]=QJsonArray{QJsonObject{{"label","image"},{"type","picture"},{"score",.8}}};
        server.areas={area("manual-overlap","picture",QRect(0,0,70,70),.99,"image"),text,
                      area("overlap","picture",QRect(90,20,50,50),.7,"image"),area("chart","picture",QRect(0,130,100,40),.8,"chart")};
        QSettings("YAGF","YAGF Revival").setValue("layout/url",server.url().toString());
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
        QVERIFY(use0 && use1 && use2); QVERIFY(use0->isEnabled());
        QVERIFY(use0->isChecked() && use1->isChecked() && use2->isChecked());
        QCOMPARE(dialog.regions().size(),5);
        use0->click(); use2->click(); QVERIFY(use1->isChecked());
        use2->click(); QVERIFY(use1->isChecked()); use2->click(); QVERIFY(use1->isChecked());
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
    void reopenedLayoutKeepsAppliedAreasUntilAnalysisIsRequested() {
        Server server;
        server.areas={area("new","text",{80,0,100,60},.95)};
        QSettings("YAGF","YAGF Revival").setValue("layout/url",server.url().toString());
        const core::ImageRegion saved{"saved",{20,30,60,40},{},false,"math",true,"display_formula",.92};
        ui::AiPageLayoutDialog dialog(sample(),{saved});
        dialog.show();
        QTRY_VERIFY(dialog.findChild<QLabel *>("layoutMessage")->text().contains("applied layout"));
        QCOMPARE(server.requests,0);
        const auto existing=dialog.regions();
        QCOMPARE(existing.size(),1);
        QCOMPARE(existing.first().id,saved.id);
        QCOMPARE(existing.first().bounds,saved.bounds);
        QCOMPARE(existing.first().content,saved.content);
        auto *apply=dialog.findChild<QPushButton *>("applyPageAreas");
        QVERIFY(!apply->isEnabled());
        dialog.findChild<QPushButton *>("analyzePageAreas")->click();
        QTRY_VERIFY_WITH_TIMEOUT(apply->isEnabled(),3000);
        QCOMPARE(server.requests,1);
        QCOMPARE(dialog.regions().first().content,"text");
    }

    void failedAnalysisCannotApply() {
        Server server; server.change=[](QJsonObject &root){root["width"]=500;}; QSettings("YAGF","YAGF Revival").setValue("layout/url",server.url().toString());
        ui::AiPageLayoutDialog dialog(sample(),{}); dialog.show(); auto *analyze=dialog.findChild<QPushButton *>("analyzePageAreas"); analyze->click();
        QTRY_VERIFY_WITH_TIMEOUT(analyze->isEnabled(),3000); QVERIFY(!dialog.findChild<QPushButton *>("applyPageAreas")->isEnabled()); QVERIFY(dialog.regions().isEmpty());
        QVERIFY(dialog.findChild<QLabel *>("layoutMessage")->text().contains("incompatible"));
    }
    void scoreSliderFiltersCachedResultsAndPreservesReview() {
        Server server;server.areas={area("high","text",{80,0,100,60},.9),
            area("low","picture",{0,130,100,40},.3,"image"),area("boundary","math",{0,80,50,30},.5,"display_formula")};
        QSettings settings("YAGF","YAGF Revival");settings.setValue("layout/url",server.url().toString());settings.setValue("layout/threshold",.5);
        ui::AiPageLayoutDialog dialog(sample(),{{"manual",{0,0,20,20},"rus",false,"text",false}});dialog.show();
        auto *slider=dialog.findChild<QSlider *>("layoutThreshold");auto *list=dialog.findChild<QListWidget *>("layoutAreas");
        auto *preview=dialog.findChild<QGraphicsView *>("layoutPreview");auto *apply=dialog.findChild<QPushButton *>("applyPageAreas");
        QVERIFY(slider && list && preview);QTRY_VERIFY_WITH_TIMEOUT(apply->isEnabled(),3000);
        const auto outlines=[&]{int count=0;for(auto *item:preview->scene()->items())if(qgraphicsitem_cast<QGraphicsRectItem *>(item) && item->isVisible())++count;return count;};
        QCOMPARE(server.requests,1);QCOMPARE(server.thresholds,QVector<double>{.1});QCOMPARE(list->count(),3);
        QCOMPARE(outlines(),3);
        QVERIFY(slider->mapTo(&dialog,QPoint()).y()>=preview->mapTo(&dialog,QPoint(0,preview->height())).y());
        auto *type=dialog.findChild<QComboBox *>("layoutType0");type->setCurrentIndex(type->findData("picture"));
        dialog.findChild<QCheckBox *>("layoutUseArea0")->click();
        slider->setValue(95);QCOMPARE(list->count(),1);QCOMPARE(outlines(),1);QVERIFY(!apply->isEnabled());
        QVERIFY(dialog.findChild<QLabel *>("layoutMessage")->text().contains("Lower the slider"));
        slider->setValue(50);QCOMPARE(list->count(),3);
        QCOMPARE(dialog.findChild<QComboBox *>("layoutType0")->currentData().toString(),"picture");
        QVERIFY(!dialog.findChild<QCheckBox *>("layoutUseArea0")->isChecked());
        slider->setValue(30);QCOMPARE(list->count(),4);QCOMPARE(dialog.regions().size(),3);
        QCOMPARE(dialog.findChild<QLabel *>("layoutThresholdValue")->text(),"30%");
        QCOMPARE(settings.value("layout/threshold").toDouble(),.3);
        slider->setValue(10);slider->setValue(70);slider->setValue(30);
        QCOMPARE(list->count(),4);QCOMPARE(outlines(),4);QCOMPARE(server.requests,1);
        auto merged=dialog.regions();QCOMPARE(merged[0].id,"manual");QCOMPARE(merged[0].language,"rus");
        list->item(3)->setCheckState(Qt::Unchecked);slider->setValue(70);slider->setValue(30);
        QVERIFY(!dialog.findChild<QCheckBox *>("layoutUseArea2")->isChecked());
        server.areas={area("replacement","text",{50,70,70,30},.7)};
        auto *analyze=dialog.findChild<QPushButton *>("analyzePageAreas");analyze->click();
        QTRY_VERIFY_WITH_TIMEOUT(server.requests==2 && analyze->isEnabled(),3000);
        slider->setValue(10);QCOMPARE(list->count(),2);QCOMPARE(dialog.regions().size(),2);
    }
    void scoreFilteringRegroupsInlineFormulasFromRawEvidence() {
        Server server;server.areas={area("text","text",{50,0,130,100},.9),
            area("inline","mixed",{70,20,20,20},.3,"inline_formula")};
        QSettings settings("YAGF","YAGF Revival");settings.setValue("layout/url",server.url().toString());settings.setValue("layout/threshold",.5);
        ui::AiPageLayoutDialog dialog(sample(),{});dialog.show();auto *apply=dialog.findChild<QPushButton *>("applyPageAreas");
        auto *slider=dialog.findChild<QSlider *>("layoutThreshold");QTRY_VERIFY_WITH_TIMEOUT(apply->isEnabled(),3000);
        QCOMPARE(dialog.regions().size(),1);QCOMPARE(dialog.regions()[0].content,"text");
        slider->setValue(30);QCOMPARE(dialog.regions().size(),1);QCOMPARE(dialog.regions()[0].content,"mixed");
        slider->setValue(31);QCOMPARE(dialog.regions()[0].content,"text");QCOMPARE(server.requests,1);
    }
    void selectedBlockSplitsTwoInlineExpressionsOnOneLine()
    {
        Server server;
        QImage image(200,70,QImage::Format_RGB32);
        image.fill(Qt::white);
        const QRect a(44,23,14,22), b(123,22,19,22);
        QPainter ink(&image);
        for (const QRect box : {QRect(2,4,190,8),QRect(3,28,35,9),a,QRect(66,28,48,9),
                                b,QRect(154,28,38,9),QRect(3,55,180,9)}) ink.fillRect(box,Qt::black);
        ink.end();
        // A broad alternative overlaps the better paragraph. The weaker duplicate
        // formula comes first in wire order and snaps to the same ink as "first".
        server.areas={area("fallback","text",image.rect(),.89),
                      area("paragraph","text",image.rect().adjusted(1,1,-1,-1),.95),
                      area("partial","mixed",a.adjusted(2,0,-2,0),.71,"inline_formula"),
                      area("first","mixed",a,.85,"inline_formula"),area("second","mixed",b,.8,"inline_formula")};
        QSettings settings("YAGF","YAGF Revival");
        settings.setValue("layout/url",server.url().toString());
        settings.setValue("layout/threshold",.5);
        ui::AiPageLayoutDialog dialog(image,{},nullptr,1,{},true);
        dialog.show();
        QTRY_VERIFY(dialog.findChild<QPushButton *>("applyPageAreas")->isEnabled());
        QCOMPARE(dialog.regions().size(),8);
        QCOMPARE(dialog.regions().first().bounds,image.rect());
        auto *keepFallback=dialog.findChild<QCheckBox *>("layoutUseArea0");
        QVERIFY(keepFallback && keepFallback->isChecked());
        keepFallback->click();
        const auto parts=dialog.regions();
        QCOMPARE(parts.size(),7);
        QCOMPARE(parts[2].content,"math"); QCOMPARE(parts[4].content,"math");
        QCOMPARE(parts[2].detectionScore,.85); QCOMPARE(parts[4].detectionScore,.8);
        QCOMPARE(parts[1].detectionScore,-1.);
        QVERIFY(parts[2].bounds.contains(a)); QVERIFY(parts[4].bounds.contains(b));
        for (int i=1; i<=5; ++i) {
            QVERIFY(parts[i].bounds.contains(parts[i].bounds.center().x(),32));
            if (i>1) QVERIFY(parts[i-1].bounds.right()<parts[i].bounds.left());
        }
        auto *slider=dialog.findChild<QSlider *>("layoutThreshold");
        slider->setValue(90); QCOMPARE(dialog.regions().size(),1);
        slider->setValue(50); QCOMPARE(dialog.regions().size(),7);
        QCOMPARE(server.requests,1);
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
        QSettings("YAGF","YAGF Revival").setValue("layout/url",url);
        ui::AiPageLayoutDialog dialog(image,{}); dialog.show();
        auto *apply=dialog.findChild<QPushButton *>("applyPageAreas");
        QTRY_VERIFY_WITH_TIMEOUT(apply->isEnabled(),120000);
        QVERIFY(std::ranges::any_of(dialog.regions(),[](const auto &r){return r.content=="picture" && r.notText;}));
        QVERIFY(dialog.grab().save("/tmp/yagf-ai-layout-dialog.png"));
    }
};
QTEST_MAIN(PageLayoutTests)
#include "aipagelayout_tests.moc"

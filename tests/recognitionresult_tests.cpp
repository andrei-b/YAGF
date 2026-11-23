#include "iocrengine.h"
#include "paddletable.h"
#include "remoteocrengine.h"
#include <QJsonDocument>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>

using namespace yagf;
namespace {
core::RecognitionResult markdownResult() {
    core::RecognitionResult result; result.engineId="document-model"; result.modelVersion="revision-1";
    result.markdown="# Heading\n\nA paragraph.\n\n| A | B |\n|---|---|\n| 1 | 2 |";
    result.structuredOutput={{"native",QJsonObject{{"pages",QJsonArray{1}}}}};
    result.provenance={.model="document-vlm",.task="ocr",.language="eng",.pageNumber=1,.imageSize={200,200}};
    return result;
}
class NativeEngine : public processing::IOcrEngine {
public:
    QString id() const override { return "document-model"; }
    processing::EngineCapabilities capabilities() const override { return {.supportsLayout=true,.supportsTables=true}; }
    std::expected<core::RecognitionResult,processing::OcrError> recognizeResult(
        const QImage &,const processing::OcrOptions &,std::stop_token) override { return markdownResult(); }
};
class NativeServer : public QTcpServer {
public:
    QJsonObject response;
    NativeServer() {
        if (!listen(QHostAddress::LocalHost)) qFatal("listen failed");
        connect(this,&QTcpServer::newConnection,this,[this] {
            auto *socket=nextPendingConnection(); connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
            connect(socket,&QTcpSocket::readyRead,socket,[this,socket,data=QByteArray{}]() mutable {
                data+=socket->readAll(); const int end=data.indexOf("\r\n\r\n");
                if (end<0 || socket->property("sent").toBool()) return;
                int length=0; for (const auto &line:data.left(end).split('\n'))
                    if (line.toLower().startsWith("content-length:")) length=line.mid(15).trimmed().toInt();
                if (data.size()<end+4+length) return;
                socket->setProperty("sent",true);
                const auto body=QJsonDocument(response).toJson(QJsonDocument::Compact);
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: "+
                    QByteArray::number(body.size())+"\r\n\r\n"+body); socket->disconnectFromHost();
            });
        });
    }
};
}
class RecognitionResultTests : public QObject {
    Q_OBJECT
private slots:
    void markdownEngineNeedsNoOcrPageOrCoordinates() {
        NativeEngine engine; QImage image(200,200,QImage::Format_RGB32); image.fill(Qt::white);
        const auto native=engine.recognizeResult(image,{},{});
        QVERIFY(native); QVERIFY(native->text.isEmpty()); QVERIFY(native->formulas.isEmpty());
        auto page=engine.recognize(image,{},{}); QVERIFY(page); QCOMPARE(page->words.size(),1);
        const auto &block=page->words[0]; QCOMPARE(block.text,native->markdown);
        QVERIFY(block.layoutBlock); QVERIFY(block.geometryInferred); QVERIFY(block.sourceBoxes.isEmpty());
        page->rebuildHierarchy(); QVERIFY(page->words[0].sourceBoxes.isEmpty());
        page->words[0].text="Edited document";
        core::OcrDocument document; document.sourcePath="scan.png"; document.replacePage(*page);
        core::OcrDocument restored; QString error; QVERIFY2(core::OcrDocument::fromJson(document.toJson(),&restored,&error),qPrintable(error));
        const auto *saved=restored.page(1); QVERIFY(saved); QCOMPARE(saved->words[0].text,"Edited document");
        QVERIFY(saved->words[0].geometryInferred); QVERIFY(saved->words[0].sourceBoxes.isEmpty());
        QCOMPARE(saved->recognitionEvidence[0].markdown,native->markdown);
        QCOMPARE(saved->recognitionEvidence[0].structuredOutput,native->structuredOutput);
        QCOMPARE(saved->recognitionEvidence[0].modelVersion,"revision-1");
    }
    void hierarchicalWordsKeepRealGeometryWithoutDuplicatedParentText() {
        auto result=markdownResult(); result.markdown.clear();
        core::TextRegion block; block.id="block"; block.text="Hello world"; block.bounds=QRect(10,10,100,20);
        block.type="sectionheader";
        core::TextRegion line; line.id="line"; line.parentId="block"; line.text="Hello world";
        line.granularity=core::TextGranularity::Line; line.bounds=block.bounds;
        core::TextRegion first; first.id="first"; first.parentId="line"; first.text="Hello";
        first.granularity=core::TextGranularity::Word; first.bounds=QRect(10,10,40,20); first.order=0;
        auto second=first; second.id="second"; second.text="world"; second.bounds=QRect(60,10,50,20); second.order=1;
        result.text={block,line,first,second};
        const auto page=processing::recognitionToPage(result); QVERIFY(page); QCOMPARE(page->words.size(),2);
        QCOMPARE(page->words[0].bounds,*first.bounds); QCOMPARE(page->words[1].bounds,*second.bounds);
        QCOMPARE(page->words[0].line,page->words[1].line); QCOMPARE(page->regions.size(),1);
        QCOMPARE(page->regions[0].type,"sectionheader"); QVERIFY(!page->words[0].layoutBlock);
        QVERIFY(!page->words[0].geometryInferred); QCOMPARE(page->recognitionEvidence[0].text.size(),4);
    }
    void orderedInlineChildKeepsParentProseAndGeometry() {
        auto result = markdownResult();
        core::TextRegion parent;
        parent.id = "p"; parent.text = "Пусть"; parent.order = 0;
        parent.granularity = core::TextGranularity::Paragraph;
        parent.bounds = QRect(0,0,100,40);
        core::FormulaRegion formula;
        formula.id = "f"; formula.parentId = "p"; formula.order = 1;
        formula.latex = "x^2"; formula.display = false;
        formula.bounds = QRect(30,0,15,15);
        result.text = {parent}; result.formulas = {formula};
        const auto page = processing::recognitionToPage(result);
        QVERIFY(page);
        QCOMPARE(page->words.size(), 2);
        QCOMPARE(page->regions.size(), 1);
        QCOMPARE(page->regions.first().type, "text");
        QCOMPARE(page->words[0].text, parent.text);
        QCOMPARE(page->words[1].text, QStringLiteral(R"(\(x^2\))"));
        QCOMPARE(page->words[1].bounds, *formula.bounds);
        QCOMPARE(page->words[1].paragraph, page->words[0].paragraph);
        const auto exported = processing::recognitionFromPage(*page);
        QVERIFY(core::validateRecognitionResult(exported));
        QCOMPARE(exported.formulas.size(), 1);
        QVERIFY(!exported.formulas.first().display);
        QCOMPARE(exported.formulas.first().parentId, parent.id);
        QCOMPARE(exported.formulas.first().latex, formula.latex);
        result.text[0].text += QStringLiteral(R"( \(x^2\))");
        const auto embedded = processing::recognitionToPage(result);
        QVERIFY(embedded);
        QCOMPARE(embedded->words.size(), 1);
    }
    void paddleMergedTableStructure() {
        const auto table = processing::parsePaddleTable(
            R"(<ched>Dimensions<lcel><fcel>Units<nl><rhed>Area<fcel>\(x^2\)<fcel>m²<nl><ucel><fcel>2x<fcel>m<nl>)");
        QVERIFY(table);
        QCOMPARE(table->text,QStringLiteral("Dimensions\tUnits\nArea\t\\(x^2\\)\tm²\n2x\tm"));
        const auto layout = table->structure["table_layout"].toArray();
        QVERIFY(core::OcrRegion::validTableLayout(layout));
        QCOMPARE(layout[0].toArray()[0].toObject()["colspan"].toInt(),2);
        QVERIFY(layout[0].toArray()[0].toObject()["header"].toBool());
        QCOMPARE(layout[1].toArray()[0].toObject()["rowspan"].toInt(),2);
        QVERIFY(!processing::parsePaddleTable("<ucel><nl>"));
        QVERIFY(!processing::parsePaddleTable("<fcel>x<fcel>y<nl><fcel>z<nl>"));
    }
    void coarseCoordinatesRemainOneBlock() {
        auto result=markdownResult(); core::TextRegion block; block.text="First line\nSecond line";
        block.bounds=QRect(10,20,100,60); block.granularity=core::TextGranularity::Block; result.text={block};
        const auto page=processing::recognitionToPage(result); QVERIFY(page); QCOMPARE(page->words.size(),1);
        QCOMPARE(page->words[0].bounds,*block.bounds); QVERIFY(page->words[0].layoutBlock); QVERIFY(!page->words[0].geometryInferred);
    }
    void knownCropExtentDoesNotBecomeModelGeometry() {
        auto result=markdownResult(); core::TextRegion region; region.text="A crop transcription";
        region.inputArea=QRect(20,30,80,40); result.text={region};
        const auto page=processing::recognitionToPage(result); QVERIFY(page); QCOMPARE(page->words.size(),1);
        QCOMPARE(page->words[0].bounds,*region.inputArea); QVERIFY(page->words[0].geometryInferred);
        QVERIFY(page->words[0].sourceBoxes.isEmpty()); QVERIFY(!page->recognitionEvidence[0].text[0].bounds);
    }
    void formulasTablesAndFiguresRetainStructureAndOrder() {
        auto result=markdownResult();
        core::TextRegion text; text.text="Heading"; text.bounds=QRect(0,0,100,20); text.order=0;
        core::FormulaRegion formula; formula.latex="\\frac{1}{2}"; formula.bounds=QRect(0,30,100,20); formula.order=1;
        core::TableRegion table; table.text="A\tB\n1\t2"; table.markdown="| A | B |\n|---|---|\n| 1 | 2 |";
        table.html="<table></table>"; table.bounds=QRect(0,60,100,40); table.order=2;
        table.structure={{"cells",QJsonArray{QJsonObject{{"row",0},{"column",0},{"row_span",2}}}}};
        core::FigureRegion figure; figure.id="figure"; figure.bounds=QRect(0,110,100,50); figure.caption="A diagram"; figure.order=3;
        result.text={text}; result.formulas={formula}; result.tables={table}; result.figures={figure};
        auto page=processing::recognitionToPage(result); QVERIFY(page); QCOMPARE(page->words.size(),4);
        QCOMPARE(page->words[1].text,formula.latex); QCOMPARE(page->regions[1].type,"equation");
        QCOMPARE(page->regions[2].type,"table"); QCOMPARE(page->imageRegions.size(),1); QVERIFY(page->imageRegions[0].notText);
        const auto json=core::recognitionResultJson(result); const auto restored=core::readRecognitionResult(json);
        QVERIFY(restored); QCOMPARE(core::recognitionResultJson(*restored),json);
    }
    void cropProvenancePreservesNativeCoordinates() {
        auto result=markdownResult(); result.provenance.imageSize={50,30};
        const auto reading=processing::recognitionToPage(result); QVERIFY(reading);
        core::OcrPage page; processing::appendRecognitionEvidence(page,*reading,{20,40});
        QCOMPARE(page.recognitionEvidence[0].provenance.imageSize,QSize(50,30));
        QCOMPARE(page.recognitionEvidence[0].provenance.inputArea,QRect(20,40,50,30));
        processing::appendRecognitionEvidence(page,*reading,{20,40}); QCOMPARE(page.recognitionEvidence.size(),1);
        QVERIFY(page.recognitionEvidence[0].text.isEmpty());
    }
    void figuresWithoutTextOrModelIdsSurviveProjects() {
        auto result=markdownResult(); result.markdown.clear();
        core::FigureRegion figure; figure.bounds=QRect(20,30,80,50); result.figures={figure};
        const auto page=processing::recognitionToPage(result); QVERIFY(page); QVERIFY(page->words.isEmpty());
        QCOMPARE(page->imageRegions.size(),1); QVERIFY(!page->imageRegions[0].id.isEmpty());
        core::OcrDocument document; document.sourcePath="scan.png"; document.replacePage(*page);
        core::OcrDocument restored; QString error;
        QVERIFY2(core::OcrDocument::fromJson(document.toJson(),&restored,&error),qPrintable(error));
        QCOMPARE(restored.page(1)->recognitionEvidence[0].figures.size(),1);
        QVERIFY(restored.page(1)->recognitionEvidence[0].figures[0].caption.isEmpty());
    }
    void remoteWorkersCanReturnMarkdownOrRichEvidence() {
        NativeServer server; server.response={{"protocol","yagf-ocr/1"},{"engine_id","native"},{"width",200},{"height",200},
            {"markdown",markdownResult().markdown},{"model","document-vlm"},{"model_version","revision-1"}};
        processing::RemoteOcrEngine engine(QUrl(QString("http://127.0.0.1:%1").arg(server.serverPort())),{},processing::RemoteOcrEngine::Provider::Native);
        QImage image(200,200,QImage::Format_RGB32); image.fill(Qt::white);
        const auto markdown=engine.recognizeResult(image,{},{}); QVERIFY(markdown); QVERIFY(markdown->text.isEmpty());
        QCOMPARE(markdown->markdown,markdownResult().markdown);
        server.response["recognition"]=core::recognitionResultJson(markdownResult());
        const auto native=engine.recognizeResult(image,{},{}); QVERIFY(native); QCOMPARE(native->engineId,"native-remote");
        const auto page=engine.recognize(image,{},{}); QVERIFY(page); QVERIFY(page->words[0].geometryInferred);
        auto wrong=markdownResult(); wrong.provenance.imageSize={199,200};
        server.response["recognition"]=core::recognitionResultJson(wrong); QVERIFY(!engine.recognizeResult(image,{},{}));
    }
    void rejectsInvalidEvidence_data() {
        QTest::addColumn<QString>("kind");
        for (const auto &kind:{"outside","cycle","duplicate","parent","confidence","granularity","size","box-type"}) QTest::newRow(kind)<<QString(kind);
    }
    void rejectsInvalidEvidence() {
        QFETCH(QString,kind); auto result=markdownResult(); core::TextRegion region; region.id="one"; region.text="Text";
        result.text={region};
        if (kind=="outside") result.text[0].bounds=QRect(199,1,10,10);
        if (kind=="cycle") result.text[0].parentId="one";
        if (kind=="duplicate") result.text.append(region);
        if (kind=="parent") result.text[0].parentId="missing";
        if (kind=="confidence") result.text[0].confidence=101;
        if (kind=="granularity") result.text[0].granularity=core::TextGranularity(-1);
        if (kind=="size") result.provenance.imageSize={0,0};
        if (kind=="box-type") {
            auto json=core::recognitionResultJson(result); auto text=json["text"].toArray(); auto item=text[0].toObject();
            item["box"]=QJsonArray{1,2,"3",4}; text[0]=item; json["text"]=text;
            QVERIFY(!core::readRecognitionResult(json)); return;
        }
        QVERIFY(!core::validateRecognitionResult(result)); QVERIFY(!processing::recognitionToPage(result));
    }
};
QTEST_GUILESS_MAIN(RecognitionResultTests)
#include "recognitionresult_tests.moc"

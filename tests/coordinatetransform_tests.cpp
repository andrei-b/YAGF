#include "coordinatetransform.h"
#include "pagegeometry.h"
#include "imageprocessor.h"
#include "correctionretention.h"
#include <QJsonDocument>
#include <QPainter>
#include <QtTest>

using namespace yagf;
class CoordinateTransformTests:public QObject {
    Q_OBJECT
    static core::OcrPage page() {
        core::OcrPage p;p.width=p.sourceWidth=200;p.height=p.sourceHeight=120;p.coordinates.sourceSize={200,120};
        p.words={{1,1,1,1,{30,20,40,12},"teh","the",95,true},{1,2,1,1,{30,60,70,15},"x=1","x=1",95}};
        p.rebuildHierarchy();p.regions[1].type="equation";
        p.imageRegions={{"manual",{20,10,100,80},"eng",false,"mixed"}};
        p.deletedWords={p.words[0]};p.deletedWords[0].id="deleted";p.deletedWords[0].text.clear();
        p.pendingCorrections={p.words[0]};p.pendingCorrections[0].id="pending";
        core::OcrReading r;r.source="other";r.engine="native";r.label="Other";r.area={0,0,200,120};
        r.items={{{30,20,40,12},"text",1,1,1,false}};p.ocrReadings={r};return p;
    }
private slots:
    void rasterAndCoordinatesAgree_data() {
        QTest::addColumn<int>("turns");QTest::addColumn<double>("angle");QTest::addColumn<double>("bend");
        for(int turns=0;turns<4;++turns)for(double angle:{0.,-4.3,6.2})for(double bend:{0.,-3.5,4.})
            QTest::newRow(qPrintable(QString("%1/%2/%3").arg(turns).arg(angle).arg(bend)))<<turns<<angle<<bend;
    }
    void rasterAndCoordinatesAgree() {
        QFETCH(int,turns);QFETCH(double,angle);QFETCH(double,bend);
        QImage source(240,160,QImage::Format_RGB32);source.fill(Qt::white);
        {QPainter painter(&source);painter.fillRect(QRect(70,50,9,9),Qt::black);}
        const core::PreprocessSettings settings{.deskewDegrees=angle,.quarterTurns=turns,.bendPercent=bend};
        auto image=processing::ImageProcessor::apply(source,settings);QVERIFY(image);
        auto mapping=processing::ImageProcessor::coordinatePipeline(source.size(),settings);QVERIFY(mapping);
        QCOMPARE(mapping->outputSize(),QSizeF(image->size()));
        auto projected=mapping->fromSource({74.5,54.5});QVERIFY(projected);
        QVERIFY(image->rect().contains(projected->toPoint()));QVERIFY(qGray(image->pixel(projected->toPoint()))<128);
        auto original=mapping->toSource(*projected);QVERIFY(original);QVERIFY(QLineF(*original,{74.5,54.5}).length()<1e-5);
    }
    void projectiveAndMeshRoundTrip() {
        core::CoordinatePipeline p;p.sourceSize={200,120};
        const QTransform projective(1,.02,.0004,.03,1,.0001,5,7,1);
        p.appendProjective("perspective",projective,{220,140});
        core::CoordinateStep s;s.operation="dewarp";s.inputSize={220,140};s.outputSize={220,150};s.gridSize={3,3};
        for(int row=0;row<3;++row)for(int col=0;col<3;++col)s.displacement.append({0,col==1?double(row+3):0.});
        p.steps.append(s);QVERIFY(p.validate());
        for(int x=10;x<180;x+=17)for(int y=10;y<110;y+=13) {
            auto out=p.fromSource({double(x),double(y)});QVERIFY(out);auto back=p.toSource(*out);QVERIFY(back);
            QVERIFY(QLineF(*back,QPointF(x,y)).length()<1e-5);
        }
        auto restored=core::CoordinatePipeline::fromJson(p.toJson());QVERIFY(restored);QCOMPARE(restored->toJson(),p.toJson());
    }
    void preserveAndUndoGeometryWithoutAccumulatingBoxGrowth() {
        const auto original=page();auto current=original;
        for(int pass=0;pass<8;++pass) {
            auto rotated=processing::ImageProcessor::coordinatePipeline({200,120},{.quarterTurns=1});QVERIFY(rotated);
            auto mapped=core::remapPageGeometry(current,*rotated);QVERIFY2(mapped,qPrintable(mapped?QString():mapped.error()));
            QCOMPARE(mapped->words[0].id,original.words[0].id);QCOMPARE(mapped->words[0].text,"the");QVERIFY(mapped->words[0].reviewed);
            QCOMPARE(mapped->words[0].bounds,QRect(88,30,12,40));QCOMPARE(mapped->deletedWords.size(),1);QCOMPARE(mapped->pendingCorrections.size(),1);
            QCOMPARE(mapped->regions[1].type,"equation");QCOMPARE(mapped->ocrReadings[0].items[0].bounds,mapped->words[0].bounds);
            auto back=core::remapPageGeometry(*mapped,original.coordinates);QVERIFY(back);current=*back;
            QCOMPARE(current.words[0].bounds,original.words[0].bounds);QCOMPARE(current.words[0].sourceBoxes,original.words[0].sourceBoxes);
            QCOMPARE(current.imageRegions[0].bounds,original.imageRegions[0].bounds);
        }
        core::OcrDocument document;document.sourcePath="scan.png";document.replacePage(current);
        core::OcrDocument restored;QString error;QVERIFY2(core::OcrDocument::fromJson(document.toJson(),&restored,&error),qPrintable(error));
        QCOMPARE(restored.page(1)->coordinates.toJson(),current.coordinates.toJson());QVERIFY(!restored.page(1)->sourceGeometry.isEmpty());
    }
    void editedGeometryReanchorsAndCorrectionsSurviveRecognition() {
        auto old=page();auto rotation=processing::ImageProcessor::coordinatePipeline({200,120},{.quarterTurns=1});QVERIFY(rotation);
        auto mapped=core::remapPageGeometry(old,*rotation);QVERIFY(mapped);
        mapped->words[0].bounds.translate(2,3);for(auto &box:mapped->words[0].sourceBoxes)box.translate(2,3);
        auto back=core::remapPageGeometry(*mapped,old.coordinates);QVERIFY(back);
        QVERIFY(back->words[0].bounds!=old.words[0].bounds);
        core::OcrPage fresh=*mapped;fresh.words[0].text="teh";fresh.words[0].reviewed=false;fresh.pendingCorrections.clear();fresh.deletedWords.clear();
        old.pendingCorrections.clear();old.deletedWords.clear();
        const auto retained=core::retainCorrections(fresh,old);QCOMPARE(retained.retained,1);QCOMPARE(fresh.words[0].text,"the");QVERIFY(fresh.words[0].reviewed);
    }
    void nativeEvidenceKeepsItsFrameWhenThePageChanges() {
        auto original=page();core::RecognitionResult result;result.engineId="native";
        result.provenance={.task="ocr",.pageNumber=1,.imageSize={50,30},.inputArea={20,10,50,30}};
        core::TextRegion region;region.text="text";region.bounds=QRect(5,5,20,10);result.text={region};original.recognitionEvidence={result};
        core::attachRecognitionCoordinates(original);const auto frozen=core::recognitionResultJson(original.recognitionEvidence[0]);
        auto rotation=processing::ImageProcessor::coordinatePipeline({200,120},{.quarterTurns=1});QVERIFY(rotation);
        auto mapped=core::remapPageGeometry(original,*rotation);QVERIFY(mapped);
        QCOMPARE(core::recognitionResultJson(mapped->recognitionEvidence[0]),frozen);
        auto outline=core::recognitionOutlineOnPage(mapped->recognitionEvidence[0],region,mapped->coordinates);QVERIFY(outline);
        QCOMPARE(core::enclosingPixels(*outline),QRect(95,25,10,20));
    }
    void pdfPointsAndRenderResolutionAreExplicit() {
        auto mapping=processing::ImageProcessor::coordinatePipeline({600,900},{},300);QVERIFY(mapping);
        QCOMPARE(mapping->sourceUnit,"page-points-top-left");QCOMPARE(mapping->steps[0].operation,"render");
        auto pixel=mapping->fromSource({72,72});QVERIFY(pixel);QVERIFY(QLineF(*pixel,{300,300}).length()<1e-6);
        auto original=mapping->toSource(*pixel);QVERIFY(original);QVERIFY(QLineF(*original,{72,72}).length()<1e-6);
        auto old=page();old.width=old.sourceWidth=600;old.height=old.sourceHeight=900;old.coordinates=*mapping;
        auto differentDpi=processing::ImageProcessor::coordinatePipeline({401,601},{},200);QVERIFY(differentDpi);
        auto mapped=core::remapPageGeometry(old,*differentDpi);QVERIFY(mapped);
        auto physical=mapped->coordinates.fromSource({72,72});QVERIFY(physical);QVERIFY(QLineF(*physical,{200,200}).length()<1e-6);
    }
    void largeFieldsAreSharedAcrossFormulaCropsInProjects() {
        auto p=page();core::CoordinateStep field;field.operation="dewarp";field.inputSize=field.outputSize={200,120};field.gridSize={129,129};
        for(int row=0;row<129;++row)for(int col=0;col<129;++col)
            field.displacement.append({0,2*std::sin(col*3.141592653589793/128)*std::sin(row*3.141592653589793/128)});
        p.coordinates.steps={field};QVERIFY(p.coordinates.validate());
        for(int i=0;i<60;++i) {
            core::RecognitionResult result;result.engineId="formula";
            result.provenance={.task="formula",.pageNumber=1,.imageSize={10,10},.inputArea={i,20,10,10}};
            core::FormulaRegion formula;formula.latex="x=1";result.formulas={formula};p.recognitionEvidence.append(result);
        }
        core::attachRecognitionCoordinates(p);
        core::OcrDocument document;document.sourcePath="scan.png";document.replacePage(p);
        const auto json=document.toJson();const auto saved=json["pages"].toArray()[0].toObject();
        QCOMPARE(saved["recognition_frames"].toObject().size(),1);
        const auto standalone=QJsonDocument(core::recognitionResultJson(p.recognitionEvidence[0])).toJson(QJsonDocument::Compact).size();
        QVERIFY(QJsonDocument(json).toJson(QJsonDocument::Compact).size()<standalone*5);
        core::OcrDocument loaded;QString error;QVERIFY2(core::OcrDocument::fromJson(json,&loaded,&error),qPrintable(error));
        QCOMPARE(loaded.page(1)->recognitionEvidence.size(),60);
        QCOMPARE(core::recognitionResultJson(loaded.page(1)->recognitionEvidence[59]),core::recognitionResultJson(p.recognitionEvidence[59]));
    }
    void rejectsSingularFoldedAndIncompatibleTransforms() {
        core::CoordinatePipeline invalid;invalid.sourceSize={200,120};invalid.appendProjective("bad",QTransform::fromScale(0,1),{200,120});QVERIFY(!invalid.validate());
        auto p=page();auto target=p.coordinates;target.sourceFrame="restored";QVERIFY(!core::remapPageGeometry(p,target));
        core::CoordinateStep folded;folded.operation="dewarp";folded.inputSize=folded.outputSize={200,120};folded.gridSize={2,2};
        folded.displacement={{0,0},{-300,0},{0,0},{-300,0}};invalid=p.coordinates;invalid.steps={folded};QVERIFY(!invalid.validate());
        QVERIFY(!core::CoordinatePipeline::fromJson(invalid.toJson()));
        QCOMPARE(p.words[0].text,"the");QCOMPARE(p.words[0].bounds,QRect(30,20,40,12));
    }
};
QTEST_GUILESS_MAIN(CoordinateTransformTests)
#include "coordinatetransform_tests.moc"

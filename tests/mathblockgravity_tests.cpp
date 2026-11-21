#include "mathblockgravity.h"
#include "mathgravityfixture.h"
#include <QtTest>

using namespace yagf;
class MathBlockGravityTests:public QObject {
    Q_OBJECT
private slots:
    void recoversIntegralLimitAndTrimsOnlyEmptyTextMargin() {
        const auto original=tests::mathGravityRegions();const auto image=tests::mathGravityImage();
        auto result=processing::refineMathBlocks(image,original);QVERIFY(result);
        QCOMPARE(result->movedComponents,1);QCOMPARE(result->changedBlocks,2);
        QCOMPARE(result->regions[1].bounds,QRect(70,75,100,65));
        QCOMPARE(result->regions[0].bounds,QRect(20,20,175,55));
        QVERIFY(!result->regions[0].bounds.intersects(result->regions[1].bounds));
        for(int i=0;i<original.size();++i) {
            QCOMPARE(result->regions[i].id,original[i].id);QCOMPARE(result->regions[i].language,original[i].language);
            QCOMPARE(result->regions[i].content,original[i].content);QVERIFY(!result->regions[i].autoDetected);
        }
        auto again=processing::refineMathBlocks(image,result->regions);QVERIFY(again);QCOMPARE(again->changedBlocks,0);
        QCOMPARE(original[0].bounds,QRect(20,20,175,65));
    }
    void usesInkDistanceInsteadOfRectangleDistance() {
        auto image=tests::mathGravityImage();QPainter painter(&image);
        painter.fillRect(QRect(70,85,100,55),Qt::white);painter.fillRect(QRect(74,99,8,24),Qt::black);
        painter.end();auto result=processing::refineMathBlocks(image,tests::mathGravityRegions());
        QVERIFY(result);QCOMPARE(result->movedComponents,0);
    }
    void preservesAccentsAndUnrelatedTextInTheExpansion() {
        auto image=tests::mathGravityImage();QPainter painter(&image);
        // A full-height neighbor in the proposed new formula rectangle cannot be
        // removed by shortening the paragraph without cutting real prose.
        painter.fillRect(QRect(75,67,20,17),Qt::black);painter.end();
        auto result=processing::refineMathBlocks(image,tests::mathGravityRegions());QVERIFY(result);QCOMPARE(result->changedBlocks,0);
        image=tests::mathGravityImage();QPainter accent(&image);accent.fillRect(QRect(114,76,6,8),Qt::white);
        accent.fillRect(QRect(110,63,3,4),Qt::black);accent.end();
        result=processing::refineMathBlocks(image,tests::mathGravityRegions());QVERIFY(result);QCOMPARE(result->changedBlocks,0);
    }
    void protectsPicturesAndRequiresFormulaBlocks() {
        auto regions=tests::mathGravityRegions();regions[0].notText=true;regions[0].content="picture";
        auto result=processing::refineMathBlocks(tests::mathGravityImage(),regions);QVERIFY(result);QCOMPARE(result->changedBlocks,0);
        regions=tests::mathGravityRegions();regions[1].content="mixed";
        result=processing::refineMathBlocks(tests::mathGravityImage(),regions);QVERIFY(result);QCOMPARE(result->changedBlocks,0);
    }
    void protectsFaintProseOutsideTheStrongInkMask() {
        auto image=tests::mathGravityImage();QPainter painter(&image);
        painter.fillRect(QRect(75,72,20,12),QColor(230,230,230));painter.end();
        auto result=processing::refineMathBlocks(image,tests::mathGravityRegions());QVERIFY(result);QCOMPARE(result->changedBlocks,0);
    }
    void carriesSoftGlyphEdgesAndKeepsGroupedSuperscriptsTogether() {
        auto image=tests::mathGravityImage();QPainter painter(&image);
        painter.fillRect(QRect(113,75,8,10),QColor(160,160,160));painter.fillRect(QRect(114,76,6,8),Qt::black);painter.end();
        auto result=processing::refineMathBlocks(image,tests::mathGravityRegions());QVERIFY(result);QCOMPARE(result->movedComponents,1);
        QVERIFY(result->regions[1].bounds.contains(QRect(113,75,8,10)));
        image=tests::mathGravityImage();QPainter group(&image);group.fillRect(QRect(114,76,6,8),Qt::white);
        group.fillRect(QRect(114,76,3,6),Qt::black);group.fillRect(QRect(120,76,3,6),Qt::black);group.end();
        result=processing::refineMathBlocks(image,tests::mathGravityRegions());QVERIFY(result);QCOMPARE(result->movedComponents,2);
    }
    void scalesWithTheScanAndRecoversLowerLimits() {
        auto regions=tests::mathGravityRegions();const auto base=tests::mathGravityImage();
        for(auto &r:regions)r.bounds={r.bounds.x()*2,r.bounds.y()*2,r.bounds.width()*2,r.bounds.height()*2};
        auto result=processing::refineMathBlocks(base.scaled(base.size()*2,Qt::IgnoreAspectRatio,Qt::FastTransformation),regions);
        QVERIFY(result);QCOMPARE(result->movedComponents,1);QCOMPARE(result->regions[1].bounds,QRect(140,150,200,130));
        regions=tests::mathGravityRegions();for(auto &r:regions)r.bounds.moveTop(base.height()-r.bounds.y()-r.bounds.height());
        result=processing::refineMathBlocks(base.mirrored(false,true),regions);QVERIFY(result);QCOMPARE(result->movedComponents,1);
        QVERIFY(result->regions[1].bounds.bottom()>regions[1].bounds.bottom());
    }
    void leavesMarksBetweenCompetingFormulasUnchanged() {
        QImage image(220,180,QImage::Format_RGB32);image.fill(Qt::white);QPainter painter(&image);
        for(const auto &box:QVector<QRect>{{90,43,9,20},{108,43,9,20},{109,76,4,6},{105,89,3,24},{121,66,3,18}})painter.fillRect(box,Qt::black);
        painter.end();QVector<core::ImageRegion> regions{{"text",{20,20,100,65},{},false,"text"},
            {"left-math",{70,85,50,55},{},false,"math"},{"right-math",{120,65,50,20},{},false,"math"}};
        auto result=processing::refineMathBlocks(image,regions);QVERIFY(result);QCOMPARE(result->changedBlocks,0);
    }
    void rejectsInvalidGeometryAndCancellationWithoutMutatingInput() {
        const auto image=tests::mathGravityImage();auto regions=tests::mathGravityRegions();
        std::stop_source stop;stop.request_stop();QVERIFY(!processing::refineMathBlocks(image,regions,stop.get_token()));
        QCOMPARE(regions[1].bounds,QRect(70,85,100,55));
        regions[0].bounds.moveLeft(-1);QVERIFY(!processing::refineMathBlocks(image,regions));
        QVERIFY(!processing::refineMathBlocks({},{}));
    }
};
QTEST_GUILESS_MAIN(MathBlockGravityTests)
#include "mathblockgravity_tests.moc"

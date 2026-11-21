#include "formulagrowth.h"

#include <QPainter>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

using yagf::core::ImageRegion;
using yagf::processing::growFormulaBlocks;

namespace {
ImageRegion block(QString id, QRect bounds, QString kind)
{
    ImageRegion region;
    region.id = id;
    region.bounds = bounds;
    region.content = kind;
    region.language = "rus";
    region.autoDetected = true;
    return region;
}

struct Sample {
    QImage image{220,120,QImage::Format_Grayscale8};
    QVector<ImageRegion> blocks{block("top",{0,4,200,46},"text"),
        block("formula",{48,50,90,29},"math"), block("bottom",{0,79,200,35},"text")};
    Sample()
    {
        image.fill(Qt::white);
        QPainter p(&image);
        for (int x = 12; x < 190; x += 12) {
            p.fillRect(x,15,7,14,Qt::black);
            p.fillRect(x,97,7,14,Qt::black);
        }
        // Tall operator, body letters, a misplaced upper limit and i=1 below.
        p.fillRect(70,51,6,26,Qt::black);
        for (int x : {90,105,120}) p.fillRect(x,59,7,15,Qt::black);
        p.fillRect(71,39,6,7,Qt::black);
        p.fillRect(66,82,3,6,Qt::black);
        p.fillRect(72,83,5,2,Qt::black);
        p.fillRect(72,86,5,2,Qt::black);
        p.fillRect(81,81,3,7,Qt::black);
    }
};
}

class FormulaGrowthTests : public QObject {
    Q_OBJECT
private slots:
    void takesLimitsFromBothNeighbors()
    {
        Sample s;
        auto r = growFormulaBlocks(s.image,s.blocks);
        QCOMPARE(r[1].bounds,QRect(QPoint(48,39),QPoint(137,87)));
        QCOMPARE(r[0].bounds.bottom(),38);
        QCOMPARE(r[2].bounds.top(),88);
        for (int i = 0; i < r.size(); ++i) {
            QCOMPARE(r[i].id,s.blocks[i].id);
            QCOMPARE(r[i].language,s.blocks[i].language);
            QVERIFY(!r[i].autoDetected);
        }
    }

    void followsShortChainBeyondSeedReach()
    {
        QImage image(120,90,QImage::Format_Grayscale8);
        image.fill(Qt::white);
        QPainter p(&image);
        p.fillRect(34,40,6,20,Qt::black);
        p.fillRect(46,34,5,6,Qt::black);
        p.fillRect(57,34,5,6,Qt::black); // Too far from the seed, close to the first script.
        p.fillRect(76,34,5,6,Qt::black);
        p.end();
        QVector<ImageRegion> blocks{block("formula",{27,40,55,25},"math"),
                                    block("misclassified-script",{57,34,5,6},"text")};
        // The unrelated mark is inside the proposed rectangular expansion.
        QCOMPARE(growFormulaBlocks(image,blocks)[0].bounds,blocks[0].bounds);
        QPainter clean(&image);
        clean.fillRect(76,34,5,6,Qt::white);
        clean.end();
        auto r = growFormulaBlocks(image,blocks);
        QCOMPARE(r.size(),1);
        QCOMPARE(r[0].bounds.top(),34);
        std::stop_source cancelled;
        cancelled.request_stop();
        auto unchanged = growFormulaBlocks(image,blocks,cancelled.get_token());
        QCOMPARE(unchanged.size(),2);
        QCOMPARE(unchanged[0].bounds,blocks[0].bounds);
    }

    void preservesStrandedText()
    {
        Sample s;
        QPainter p(&s.image);
        p.fillRect(4,40,4,5,Qt::black); // Outside formula width, inside donor strip.
        p.end();
        auto r = growFormulaBlocks(s.image,s.blocks);
        QCOMPARE(r[1].bounds.top(),50);
        QCOMPARE(r[0].bounds,s.blocks[0].bounds);
        QCOMPARE(r[1].bounds.bottom(),87);
    }

    void protectsTextLinesAndPictures()
    {
        Sample s;
        QPainter p(&s.image);
        p.fillRect(0,37,200,12,Qt::white);
        for (int x = 50; x < 130; x += 10) p.fillRect(x,32,6,14,Qt::black);
        p.end();
        s.blocks[2].content = "picture";
        auto r = growFormulaBlocks(s.image,s.blocks);
        for (int i = 0; i < r.size(); ++i) QCOMPARE(r[i].bounds,s.blocks[i].bounds);
    }

    void tiesBetweenFormulas()
    {
        QImage image(80,80,QImage::Format_Grayscale8);
        image.fill(Qt::white);
        QPainter p(&image);
        p.fillRect(25,10,9,16,Qt::black);
        p.fillRect(25,40,9,16,Qt::black);
        p.fillRect(27,31,5,4,Qt::black);
        p.end();
        QVector<ImageRegion> blocks{block("a",{20,9,20,18},"math"),block("b",{20,39,20,18},"math")};
        for (int order = 0; order < 2; ++order) {
            auto r = growFormulaBlocks(image,blocks);
            QCOMPARE(r[0].bounds,blocks[0].bounds);
            QCOMPARE(r[1].bounds,blocks[1].bounds);
            std::swap(blocks[0],blocks[1]);
        }
    }

    void groupsNumeratorOverFractionBar()
    {
        QImage image(50,55,QImage::Format_Grayscale8);
        image.fill(Qt::white);
        QPainter p(&image);
        p.fillRect(12,9,9,13,Qt::black);
        p.fillRect(23,12,6,9,Qt::black);
        p.fillRect(7,23,26,3,Qt::black);
        p.fillRect(8,31,9,13,Qt::black);
        p.fillRect(18,35,6,9,Qt::black);
        p.end();
        QVector<ImageRegion> blocks{block("top",{0,0,36,22},"text"),
                                    block("fraction",{0,22,36,25},"math")};
        auto r = growFormulaBlocks(image,blocks);
        QCOMPARE(r[1].bounds.top(),9);
    }

    void typesetSummation()
    {
        if (QStandardPaths::findExecutable("pdftoppm").isEmpty()) QSKIP("Requires pdftoppm");
        const QString pdf = QStringLiteral(MATH_FIXTURE_DIR "/equations.pdf");
        QTemporaryDir tmp;
        const auto path = tmp.filePath("sum");
        QCOMPARE(QProcess::execute("pdftoppm",{"-f","8","-l","8","-singlefile","-r","150","-png",pdf,path}),0);
        QImage image(path+".png");
        QVERIFY(!image.isNull());
        image = image.copy(160,56,38,76);
        QVector<ImageRegion> blocks{block("upper",{0,0,38,19},"text"),
            block("sigma",{3,19,32,32},"math"),block("lower",{0,51,38,25},"text")};
        const auto r = growFormulaBlocks(image,blocks);
        QVERIFY2(r[1].bounds.top() <= 8,"The printed summation's upper n must be inside its crop.");
        QVERIFY2(r[1].bounds.bottom() >= 66,"The lower i=1 must be inside its crop.");
    }
};

QTEST_GUILESS_MAIN(FormulaGrowthTests)
#include "formulagrowth_tests.moc"

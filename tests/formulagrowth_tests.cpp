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

    void refinesInlineFractionBetweenProseFragments()
    {
        QImage image(240,125,QImage::Format_Grayscale8);
        image.fill(Qt::white);
        QPainter ink(&image);
        for (int y : {18,55,87})
            for (int x = 20; x < 205; x += 8)
                if (y != 55 || x < 75 || x > 147) ink.fillRect(x,y,6,11,Qt::black);
        ink.fillRect(101,44,8,9,Qt::black);
        ink.fillRect(96,56,20,2,Qt::black);
        ink.fillRect(102,61,7,12,Qt::black);
        ink.end();
        const auto parent = block("paragraph",{14,10,200,95},"text");
        const auto other = block("illustration",{0,106,230,15},"picture");
        const auto result = yagf::processing::refineFormulaRegions(image,{parent,other});
        QCOMPARE(result.size(),6);
        QCOMPARE(result[2].content,"math");
        QVERIFY(result[2].bounds.contains(QRect(96,44,20,29)));
        QVERIFY(result[1].bounds.right() < result[2].bounds.left());
        QVERIFY(result[3].bounds.left() > result[2].bounds.right());
        QCOMPARE(result.last().id,other.id);
        for (int i = 0; i < 5; ++i) {
            QCOMPARE(result[i].language,parent.language);
            QVERIFY(parent.bounds.contains(result[i].bounds));
            for (int j = 0; j < i; ++j) QVERIFY(!result[i].bounds.intersects(result[j].bounds));
        }
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x)
                if (image.constScanLine(y)[x] < 175)
                    QVERIFY(std::ranges::any_of(result,[&](const auto &r){return r.bounds.contains(x,y);}));
        QPainter clear(&image);
        clear.fillRect(90,40,32,36,Qt::white);
        clear.end();
        QCOMPARE(yagf::processing::refineFormulaRegions(image,{parent}).first().id,parent.id);
    }

    void unsplittableInlineExpressionKeepsMixedOcr()
    {
        QImage image(180,70,QImage::Format_Grayscale8);
        image.fill(Qt::white);
        const QRect touching(71,10,15,10), isolated(70,40,30,14);
        QPainter ink(&image);
        for (QRect box : {QRect(6,10,165,10),QRect(6,42,49,9),isolated,QRect(116,42,55,9)})
            ink.fillRect(box,Qt::black);
        ink.end();

        QVector<yagf::core::OcrWord> words;
        for (QRect box : {touching,isolated}) {
            yagf::core::OcrWord word;
            word.bounds = box;
            word.text = "x=1";
            words.append(word);
        }
        const auto parts = yagf::processing::refineFormulaRegions(image,{block("para",image.rect(),"mixed")},words);
        QCOMPARE(parts.size(),4);
        QVERIFY(parts[0].bounds.contains(touching));
        QCOMPARE(parts[0].content,"mixed");
        QCOMPARE(parts[1].content,"text");
        QCOMPARE(parts[2].content,"math");
        QCOMPARE(parts[3].content,"text");
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

    void adjacentLineContoursKeepWholeGlyphs()
    {
        QImage image(180,100,QImage::Format_Grayscale8);
        image.fill(Qt::white);
        QPainter pen(&image);
        const QRect descender(48,16,5,32), ascender(92,33,6,31);
        for (QRect ink : {descender,ascender,QRect(15,20,25,9),QRect(16,55,27,9)})
            pen.fillRect(ink,Qt::black);
        pen.end();
        QVector<ImageRegion> lines{block("upper",{10,12,150,36},"text"),
                                   block("lower",{10,33,150,36},"text")};
        yagf::processing::refineLineShapes(image,lines);
        const auto top = lines[0].shape(), bottom = lines[1].shape();
        QVERIFY(!lines[0].parts.isEmpty() && !lines[1].parts.isEmpty());
        QVERIFY(top.intersected(bottom).isEmpty());
        QVERIFY(QRegion(descender).subtracted(top).isEmpty());
        QVERIFY(QRegion(ascender).subtracted(bottom).isEmpty());
        QVERIFY(lines[0].bounds.intersects(lines[1].bounds));
        const auto crop = lines[0].crop(image);
        QCOMPARE(crop.pixelColor(ascender.topLeft()-lines[0].bounds.topLeft()),QColor(Qt::white));
        yagf::processing::refineLineShapes(image,lines);
        QCOMPARE(lines[0].shape(),top); QCOMPARE(lines[1].shape(),bottom);
        yagf::core::OcrDocument book, roundTrip; book.sourcePath="scan.png";
        yagf::core::OcrPage page; page.width=page.sourceWidth=image.width(); page.height=page.sourceHeight=image.height();
        page.imageRegions=lines; book.replacePage(page);
        QString error;
        QVERIFY2(yagf::core::OcrDocument::fromJson(book.toJson(),&roundTrip,&error),qPrintable(error));
        QCOMPARE(roundTrip.page(1)->imageRegions[0].shape(),top);
        QCOMPARE(roundTrip.page(1)->imageRegions[1].shape(),bottom);
    }

    void page69LimitProtrudesIntoTheLineAbove()
    {
        QImage scan(QStringLiteral(MATH_FIXTURE_DIR "/regressions/context/calculus-bounded-integral.png"));
        QVERIFY(!scan.isNull());
        QVector<ImageRegion> lines{block("previous",{2,0,665,45},"text"),
                                   block("formula-line",{2,38,665,56},"mixed")};
        yagf::processing::refineLineShapes(scan,lines);
        const auto shape=lines[1].shape();
        QVERIFY(QRegion(QRect(222,32,7,9)).subtracted(shape).isEmpty());
        QVERIFY(QRegion(QRect(216,82,13,12)).subtracted(shape).isEmpty());
        QVERIFY(!shape.intersects(lines[0].shape()));
        QVERIFY(lines[0].shape().contains(QPoint(130,25)));
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

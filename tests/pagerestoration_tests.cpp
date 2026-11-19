#include "pagerestoration.h"
#include "exportpage.h"
#include "projectfile.h"
#include <QBuffer>
#include <QGuiApplication>
#include <QPainter>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <cmath>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

using namespace yagf;
namespace restore = processing::restoration;

static QImage printedPage()
{
    QImage page(800, 500, QImage::Format_RGB32);
    page.fill(Qt::white);
    QPainter painter(&page);
    QFont font("DejaVu Sans");
    font.setPixelSize(16);
    painter.setFont(font);
    QRandomGenerator random(1);
    for (int y = 23; y < 490; y += 24) {
        QString line;
        for (int word = 0; word < 15; ++word)
            line += QString::number(random.generate(), 36) + " ";
        painter.drawText(6, y, line);
    }
    return page;
}

static QImage smear(const QImage &page, QRect region)
{
    QImage result = page;
    for (int y = region.top(); y <= region.bottom(); ++y) {
        for (int x = region.left(); x <= region.right(); ++x) {
            int sum = 0;
            for (int dx = -4; dx <= 4; ++dx)
                sum += qGray(page.pixel(std::clamp(x+dx, 0, page.width()-1), y));
            const int v = sum/9;
            result.setPixel(x, y, qRgb(v,v,v));
        }
    }
    return result;
}

static double difference(const QImage &a, const QImage &b, QRect region)
{
    double total = 0;
    for (int y = region.top(); y <= region.bottom(); ++y)
        for (int x = region.left(); x <= region.right(); ++x)
            total += std::abs(qGray(a.pixel(x,y))-qGray(b.pixel(x,y)));
    return total/(region.width()*region.height());
}

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    auto clean = printedPage();
    const QRect right(420, 0, 380, 500), sample(460, 60, 280, 360);
    auto first = smear(clean, right);
    auto heat = restore::sharpnessMap(first);
    assert(heat && heat->size() == clean.size());
    const auto tint = [&](int x) {
        int sum = 0;
        for (int y=20; y<450; ++y) sum += qRed(heat->pixel(x,y))-qBlue(heat->pixel(x,y));
        return sum;
    };
    assert(tint(601) > tint(101));
    auto corrected = restore::deblur(first, right, 9, 0, .8);
    assert(corrected && difference(*corrected,clean,sample) < difference(first,clean,sample));
    assert(difference(*corrected,first,{0,0,420,500}) == 0);
    QRect detail(657, 220, 142, 140); // Includes tile and selection edges.
    assert(restore::deblur(first,right,9,0,.8,detail).value() == corrected->copy(detail));
    const auto soft = restore::deblur(first, right, 6, 12, .55, {}, restore::BlurProfile::SoftFocus);
    assert(soft && *soft != *corrected);
    assert(restore::deblur(first, right, 6, 12, .55, detail, restore::BlurProfile::SoftFocus).value() == soft->copy(detail));
    auto vertical = restore::deblur(first,right,9,90,.8,detail);
    assert(vertical && *vertical != corrected->copy(detail));

    QImage second(clean.size(), clean.format());
    second.fill(Qt::white);
    { QPainter painter(&second); painter.drawImage(13, -7, smear(clean,{0,0,320,500})); }
    auto merged = restore::mergeCaptures(first,second);
    assert(merged && difference(*merged,clean,sample) < difference(first,clean,sample)*.6);
    assert(difference(*merged,clean,{20,30,260,420}) < 1);
    QImage blank(80,80,QImage::Format_RGB32);
    blank.fill(Qt::white);
    assert(!restore::mergeCaptures(first,blank));

    QVector<QPolygonF> guides{{{0,40},{100,70},{200,40}},{{0,130},{100,115},{200,130}}};
    QImage bent(201,160,QImage::Format_RGB32);
    bent.fill(Qt::white);
    { QPainter p(&bent); p.setPen(QPen(Qt::black,3)); for (const auto &g:guides) p.drawPolyline(g); }
    auto flat = restore::straighten(bent,guides);
    assert(flat && flat->size() == bent.size());
    for (int x=1; x<200; ++x) assert(qGray(flat->pixel(x,55)) < 40);
    guides[1][1].setY(35);
    assert(!restore::straighten(bent,guides));

    QTemporaryDir dir;
    assert(dir.isValid());
    core::OcrDocument doc;
    doc.sourcePath = dir.filePath("page.png");
    assert(first.save(doc.sourcePath));
    core::OcrPage page;
    page.id = "page-1";
    page.width = page.sourceWidth = first.width();
    page.height = page.sourceHeight = first.height();
    QBuffer buffer(&page.preprocessing.restoredPng);
    assert(buffer.open(QIODevice::WriteOnly) && corrected->save(&buffer,"PNG"));
    doc.replacePage(page);
    QString error;
    for (bool portable : {false, true}) {
        const QString path = dir.filePath(portable ? "bundle" : "scan.yagf.json");
        core::OcrDocument loaded;
        assert(portable ? core::ProjectFile::savePortable(path,doc,&error) : core::ProjectFile::save(path,doc,&error));
        assert(portable ? core::ProjectFile::loadPortable(path,&loaded,&error) : core::ProjectFile::load(path,&loaded,&error));
        assert(loaded.page(1)->preprocessing == page.preprocessing);
        const auto output = dir.filePath("export.png");
        assert(processing::writeScanPage(loaded,1,output,"PNG",false).status == processing::ExportResult::Status::Success);
        assert(QImage(output).convertToFormat(corrected->format()) == *corrected);
    }
}

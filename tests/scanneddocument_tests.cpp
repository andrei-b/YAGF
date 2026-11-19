#include "imageprocessor.h"
#include "pageorientation.h"
#include "tesseractengine.h"
#include "pagesource.h"
#include "exportpage.h"
#include <QGuiApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <cstdio>
#include <cmath>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

int main(int argc,char **argv)
{
    QGuiApplication app(argc,argv);
    const QString source=qEnvironmentVariable("YAGF_SCAN_REGRESSION_PDF",QStringLiteral(YAGF_SCAN_PATH));
    QFile file(source);
    if(!file.open(QIODevice::ReadOnly)) { std::puts("Private scan fixture absent; set YAGF_SCAN_REGRESSION_PDF to run this regression.");return 77; }
    QCryptographicHash hash(QCryptographicHash::Sha256);assert(hash.addData(&file));
    if(hash.result().toHex()!="6ee9337949b87bd6bf81f157e81944979ee31fe3a74b9b0be5ff91700ef7db22") {
        std::puts("Scan fixture hash differs; the regression expects the supplied five-page Scanned Document.pdf.");return 77;
    }
    for(const auto *tool:{"tesseract","pdftoppm"}) if(QStandardPaths::findExecutable(tool).isEmpty())return 77;
    using namespace yagf;
    QTemporaryDir temporary;assert(temporary.isValid());
    core::OcrDocument document;document.sourcePath=source;document.totalPages=5;
    const int rotations[]={0,3,1,0,0};const double angles[]={0,-4.9,1.5,3,4.4};
    const int minimumWords[]={0,500,500,450,260};
    std::atomic_bool cancelled=false;
    for(int number=2;number<=5;++number) {
        auto command=processing::pageRenderCommand(source,number,temporary.filePath(QString::number(number)),false,300);
        QProcess renderer;renderer.start(command.program,command.arguments);assert(renderer.waitForFinished(30000) && renderer.exitCode()==0);
        QImage raw(command.outputPath);assert(!raw.isNull());
        auto binary=processing::ImageProcessor::apply(raw,{.adaptiveBinarization=true});assert(binary);
        auto orientation=processing::detectPageOrientation(*binary);assert(orientation && orientation->quarterTurns==rotations[number-1]);
        auto upright=processing::ImageProcessor::apply(raw,{.quarterTurns=orientation->quarterTurns});assert(upright);
        auto assessment=processing::ImageProcessor::assess(*upright);assert(assessment && std::abs(assessment->skewDegrees-angles[number-1])<0.7);
        const core::PreprocessSettings settings{.deskewDegrees=assessment->skewDegrees,.quarterTurns=orientation->quarterTurns,.adaptiveBinarization=true};
        auto prepared=processing::ImageProcessor::apply(raw,settings);assert(prepared);
        const auto after=processing::ImageProcessor::assess(*prepared);assert(after && std::abs(after->skewDegrees)<0.6);
        prepared->setDotsPerMeterX(11811);prepared->setDotsPerMeterY(11811);
        processing::TesseractEngine engine;
        auto recognized=engine.recognize(*prepared,{.language=number<4?"deu":"eng",.pageNumber=number},{});
        assert(recognized && recognized->words.size()>=minimumWords[number-1]);
        if(number<4) {
            QRect extent;for(const auto &word:recognized->words)extent|=word.bounds;
            int left=0,right=0;for(const auto &word:recognized->words)(word.bounds.center().x()<extent.center().x()?left:right)++;
            assert(left>150 && right>150); // Neither photographed book page may disappear.
        }
        recognized->sourceWidth=raw.width();recognized->sourceHeight=raw.height();recognized->renderDpi=300;recognized->preprocessing=settings;document.replacePage(*recognized);
        auto exported=processing::readExportPage(document,number,temporary,cancelled);assert(exported && exported->size()==prepared->size());
        assert(*exported==*prepared); // Editor, OCR, save/reopen, and export share the transform.
        std::printf("Page %d: rotate %d, skew %.1f -> %.1f, %lld OCR items\n",number,orientation->quarterTurns*90,assessment->skewDegrees,after->skewDegrees,static_cast<long long>(recognized->words.size()));
    }
    core::OcrDocument restored;QString error;assert(core::OcrDocument::fromJson(document.toJson(),&restored,&error));
    assert(restored.toJson()==document.toJson());
}

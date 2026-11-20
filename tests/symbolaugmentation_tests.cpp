#include "symbolfixture.h"
#include "projectfile.h"
#include "documentprocessor.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QFile>
#include <QTemporaryDir>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

class SymbolFixtureEngine final : public yagf::processing::IOcrEngine {
public:
    yagf::core::OcrPage page;
    QString name="fixture";
    QString id() const override { return name; }
    yagf::processing::EngineCapabilities capabilities() const override { return {.supportsWordBoxes=true}; }
    std::expected<yagf::core::OcrPage,yagf::processing::OcrError> recognize(const QImage &,const yagf::processing::OcrOptions &,std::stop_token) override { return page; }
};

int main(int argc,char **argv)
{
    QApplication app(argc,argv); QTemporaryDir temporary; assert(temporary.isValid());
    using namespace yagf;
    auto rule=symbolRuleFixture();
    assert(!core::symbolRuleTrained(rule));
    auto trained=core::trainSymbolRule(rule); assert(trained); rule.model=*trained;
    assert(core::symbolRuleTrained(rule));
    assert(core::symbolRuleScore(rule,processing::symbolFeature(symbolFixture()))>.9);
    assert(core::symbolRuleScore(rule,processing::symbolFeature(symbolFixture(false)))==0);
    assert(core::symbolRuleScore(rule,processing::symbolFeature(symbolFixture(true,5)))>0);
    auto smiley=rule; smiley.id="smiley-rule"; smiley.label="☺";
    for (int i=0;i<3;++i) smiley.examples[i].mask=processing::symbolFeature(smileyFixture(i<2,i==1?3:2));
    auto smileyModel=core::trainSymbolRule(smiley); assert(smileyModel); smiley.model=*smileyModel;
    assert(core::symbolRuleScore(smiley,processing::symbolFeature(smileyFixture()))>0);
    assert(core::symbolRuleScore(smiley,processing::symbolFeature(smileyFixture(false)))==0);
    auto pending=rule; pending.examples[0].decision="pending"; assert(!core::trainSymbolRule(pending));
    auto conflict=rule; conflict.examples[2].mask=conflict.examples[0].mask; assert(!core::trainSymbolRule(conflict));
    auto edited=rule; edited.label="☺"; assert(!core::symbolRuleTrained(edited));
    assert(!processing::symbolLanguageMatches("rus","eng"));
    assert(processing::symbolLanguageMatches("ru","rus+eng"));
    assert(processing::symbolLanguageMatches("","deu"));
    QImage image(180,80,QImage::Format_RGB32); image.fill(Qt::white);
    { QPainter p(&image); p.drawImage(10,10,symbolFixture()); p.drawImage(65,10,symbolFixture(true,4)); p.drawImage(120,10,symbolFixture(false)); }
    core::OcrPage page; page.number=1; page.width=page.sourceWidth=image.width(); page.height=page.sourceHeight=image.height();
    page.engineId="tesseract"; page.language="rus";
    for (int i=0;i<3;++i) page.words.append({1,1,1,1,QRect(10+i*55,10,40,48),"ф","ф",45});
    page.rebuildHierarchy();
    const auto candidates=processing::findSymbolCandidates(image,rule,150);
    assert(candidates.size()>=3);
    assert(processing::applySymbolRules(page,image,{rule})==2);
    assert(page.words[0].text=="ψ" && page.words[1].text=="ψ" && page.words[2].text=="ф");
    assert(!page.words[0].reviewed && page.words[0].originalText=="ф" && page.words[0].readingEngine=="symbol-augmentation");
    page.words[0].text="ф"; page.words[0].reviewed=true;
    assert(processing::applySymbolRules(page,image,{rule})==0 && page.words[0].text=="ф");
    page.words[0].reviewed=false; page.language="eng";
    assert(processing::applySymbolRules(page,image,{rule})==0);
    page.language="rus"; auto disabled=rule; disabled.enabled=false;
    assert(processing::applySymbolRules(page,image,{disabled})==0);
    auto competing=rule; competing.id="other-rule"; competing.label="☺"; competing.model=*core::trainSymbolRule(competing);
    assert(processing::applySymbolRules(page,image,{rule,competing})==0);
    auto missing=page;
    missing.words.removeAt(1);
    missing.words[0].text=missing.words[0].originalText="ф";
    assert(processing::applySymbolRules(missing,image,{rule})==2);
    assert(missing.words.size()==3 && missing.words[1].text=="ψ" && missing.words[1].readingEngine=="symbol-augmentation");
    auto ambiguous=page; ambiguous.words.removeAt(1); ambiguous.words[1].block=2;
    assert(processing::applySymbolRules(ambiguous,image,{rule})==1 && ambiguous.words.size()==2);
    auto excluded=page; excluded.words.removeAt(1); excluded.imageRegions={{"figure",{60,0,50,75},{},true}};
    assert(processing::applySymbolRules(excluded,image,{rule})==1);
    core::OcrDocument book; book.sourcePath=temporary.filePath("source.png"); assert(image.save(book.sourcePath));
    book.engine="tesseract"; book.language="rus"; book.symbolRules={rule}; book.replacePage(page);
    QString error; const auto path=temporary.filePath("symbols.yagf.json"); assert(core::ProjectFile::save(path,book,&error));
    core::OcrDocument loaded; assert(core::ProjectFile::load(path,&loaded,&error));
    assert(core::symbolRulesJson(loaded.symbolRules)==core::symbolRulesJson(book.symbolRules));
    const auto library=temporary.filePath("library.json"); assert(core::saveSymbolLibrary({rule},library));
    const auto read=core::loadSymbolLibrary(library); assert(read && read->size()==1 && core::symbolRuleTrained(read->first()));
    // Fresh OCR jobs consume an immutable snapshot of the trained rules.
    qputenv("XDG_DATA_HOME",QFile::encodeName(temporary.path()));
    for (bool raw : {false,true}) {
        auto jobBook=book; jobBook.useRawOcrOutput=raw;
        auto provider=std::make_unique<SymbolFixtureEngine>(); provider->page=page;
        for (auto &w:provider->page.words) { w.text=w.originalText="ф"; w.reviewed=false; }
        if (raw) provider->name="tesseract";
        processing::DocumentProcessor processor(nullptr,&jobBook); processor.setEngine(std::move(provider));
        bool finished=false;
        QObject::connect(&processor,&processing::DocumentProcessor::failed,&app,[&](const QString &e) { qFatal("%s",qPrintable(e)); });
        QObject::connect(&processor,&processing::DocumentProcessor::recognized,&app,[&](const auto &reading,const auto &,const auto &) {
            finished=true; assert(reading.words[0].text==(raw ? "ф" : "ψ")); assert(reading.words[2].text=="ф");
        });
        processor.recognize(book.sourcePath,"rus",1); QElapsedTimer timer; timer.start();
        while (!finished && timer.elapsed()<10000) { QApplication::processEvents(); QThread::msleep(5); }
        assert(finished);
    }
    auto bad=core::symbolRulesJson({rule}); auto malformed=bad[0].toObject(); malformed["seed_box"]=QJsonArray{-1,0,2,2}; bad[0]=malformed;
    assert(!core::readSymbolRules(bad));
    bad=core::symbolRulesJson({rule}); malformed=bad[0].toObject(); auto model=malformed["model"].toObject(); model["radius"]=5.; malformed["model"]=model; bad[0]=malformed;
    assert(!core::readSymbolRules(bad));
    // Exact character boxes are required for embedded substitutions.
    const QString helper=temporary.filePath("tesseract"); QFile script(helper); assert(script.open(QIODevice::WriteOnly));
    script.write("#!/bin/sh\nprintf '%s\\n' 'а 6 6 16 46 0' 'Ф 16 6 36 46 0' 'б 36 6 46 46 0'\n"); script.close();
    assert(script.setPermissions(QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
    qputenv("PATH",QFile::encodeName(temporary.path())+':'+qgetenv("PATH"));
    core::OcrWord word; word.bounds={10,10,40,40}; word.originalText="аФб";
    const auto cuts=processing::symbolCuts(image,word,"rus"); assert(cuts.size()==3 && cuts[1].start==1 && cuts[1].bounds==QRect(20,10,20,40));
    word.originalText="афб"; assert(processing::symbolCuts(image,word,"rus").isEmpty());
}

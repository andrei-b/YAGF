#include "blockfont.h"
#include <QCoreApplication>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
using namespace yagf::core;
namespace {
OcrWord word(int block,int line,QRect bounds,QString text,int points,int pixels=0) {
    OcrWord w; w.block=block; w.paragraph=1; w.line=line; w.bounds=bounds;
    w.text=w.originalText=text; w.fontSizePt=w.sourceFontSizePt=points; w.sourceFontSizePx=pixels; return w;
}
OcrPage page() {OcrPage p; p.width=p.sourceWidth=800; p.height=p.sourceHeight=500; return p;}
void sizes(const OcrPage &p,const QVector<int> &indices) {
    assert(!indices.isEmpty()); const auto &first=p.words[indices.first()]; assert(first.sourceFontSizePx>0);
    for (int i:indices) {const auto &w=p.words[i]; assert(w.fontSizePt==first.fontSizePt && w.sourceFontSizePt==first.sourceFontSizePt && w.sourceFontSizePx==first.sourceFontSizePx);}
}
}
int main(int argc,char **argv)
{
    QCoreApplication app(argc,argv);
    auto p=page();
    p.words={word(1,1,{10,20,90,20},"русский",11),word(1,2,{10,60,110,24},"строка",13),
             word(1,2,{130,60,4,3},".",30),word(2,1,{400,20,180,40},"Заголовок",22)};
    p.words[0].bold=true; p.words[1].italic=true;
    p.rebuildHierarchy(); const auto original=p.words;
    normalizeBlockFontSizes(p); sizes(p,{0,1,2}); assert(p.words[3].fontSizePt==22);
    assert(p.words[0].bold && p.words[1].italic);
    for (int i=0;i<p.words.size();++i) assert(p.words[i].text==original[i].text && p.words[i].bounds==original[i].bounds && p.words[i].id==original[i].id);
    OcrDocument book; book.sourcePath="scan.png"; book.replacePage(p);
    const auto html=book.layoutHtml(); assert(html.count(QString("font-size:%1px").arg(p.words[0].sourceFontSizePx))==3);
    OcrDocument restored; QString error; assert(OcrDocument::fromJson(book.toJson(),&restored,&error)); sizes(*restored.page(1),{0,1,2});

    // One user/AI block can contain several native OCR blocks and paragraphs.
    p=page(); p.words={word(1,1,{10,10,80,18},"Первая",11,24),word(2,1,{10,60,90,22},"вторая",12,25),word(3,1,{450,10,100,36},"Heading",22,46)};
    p.imageRegions={{"body",{0,0,300,200},{},false,"text"},{"heading",{400,0,300,100},{},false,"text"}};
    p.rebuildHierarchy(); normalizeBlockFontSizes(p); sizes(p,{0,1}); assert(p.words[2].sourceFontSizePx==46);
    p.words[1].block=p.words[2].block=1; normalizeBlockFontSizes(p); assert(p.words[2].fontSizePt==22);

    // A selected crop is one block regardless of the provider's subdivisions.
    p.imageRegions.clear(); normalizeBlockFontSizes(p,{},true); sizes(p,{0,1,2});

    // A raster font measurement takes priority over heuristic line sizes, in
    // both points and native pixels (including mixed-language paragraphs).
    p=page(); p.words={word(1,1,{0,0,200,20},"неизмеренная строка",11),word(1,1,{220,0,80,35},"Latin",19,40)};
    p.words[1].fontFamily="Measured face"; p.rebuildHierarchy(); normalizeBlockFontSizes(p);
    sizes(p,{0,1}); assert(p.words[0].fontSizePt==19 && p.words[0].sourceFontSizePx==40);

    // Explicit image blocks take priority over smaller, automatic OCR hints.
    p=page(); p.words={word(1,1,{10,10,80,18},"Первая",11,24),word(2,1,{10,60,90,22},"вторая",12,25)};
    p.imageRegions={{"one",{0,0,300,50},{},false,"text"},{"two",{0,50,300,50},{},false,"text"}};
    p.rebuildHierarchy(); normalizeBlockFontSizes(p,{{"user",{0,0,300,200},{},false,"text"}}); sizes(p,{0,1});

    // Formulas and explicit font-size edits keep their own sizing. Reviewed
    // spelling corrections without a size edit still follow their text block.
    p=page(); p.words={word(1,1,{10,10,80,18},"основной",11,24),word(1,1,{100,10,40,30},"ручной",17,24),
        word(1,2,{10,60,50,14},"верно",11,18),word(1,2,{100,60,80,40},R"(\(x^2\),)",8,12)};
    p.words[1].sourceFontSizePt=11; p.words[1].reviewed=true; p.words[2].reviewed=true;
    p.rebuildHierarchy(); normalizeBlockFontSizes(p); sizes(p,{0,2});
    assert(p.words[1].fontSizePt==17 && p.words[1].sourceFontSizePt==11 && p.words[1].sourceFontSizePx==24);
    assert(p.words[3].fontSizePt==8 && p.words[3].sourceFontSizePx==12 && p.words[2].reviewed);

    // Paragraph OCR boxes and words joined over a line break must not inflate
    // the font measurement to the height of several lines.
    p=page(); p.words={word(1,1,{0,0,200,200},"Paragraph\nwith several lines",11)}; p.words[0].layoutBlock=true;
    p.rebuildHierarchy(); normalizeBlockFontSizes(p); assert(p.words[0].sourceFontSizePx==23);
    p=page(); p.words={word(1,1,{0,0,100,200},"перенесенный",11)}; p.words[0].sourceBoxes={{0,0,40,20},{0,180,100,20}};
    p.rebuildHierarchy(); normalizeBlockFontSizes(p); assert(p.words[0].sourceFontSizePx==23);
    auto snapshot=p.words[0]; std::stop_source stop; stop.request_stop(); p.words[0].sourceFontSizePx=0;
    normalizeBlockFontSizes(p,{},false,stop.get_token()); assert(p.words[0].sourceFontSizePx==0);
}

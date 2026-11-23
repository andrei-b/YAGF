#include "formulatext.h"
#include "ocrdocument.h"
#include "projectfile.h"
#include "wordeditor.h"
#include "correctionmemory.h"
#include "documentcommands.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QUndoStack>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
using namespace yagf;
int main(int argc,char **argv)
{
    QCoreApplication app(argc,argv);
    core::OcrPage notation;
    core::OcrWord token;
    token.text = "точка с координатой c, при х > 0";
    notation.words = {token};
    for (const auto &[offset, expected] : QVector<QPair<int,QString>>{
             {int(token.text.indexOf("с координатой")), "с"}, {int(token.text.indexOf("х >")), "x"}}) {
        const auto choices = core::localSymbolChoices(notation, 0, offset, 1);
        assert(!choices.isEmpty() && choices.first().source == expected);
    }
    assert(core::localSymbolChoices(notation,0,2,1).isEmpty());
    const QVector<QPair<QString,QString>> expressions{
        {R"(p_{3}=2,\ q_1=1)","p₃=2,\u2009q₁=1"},
        {R"(\psi\leq\alpha+\beta)","ψ≤α+β"},
        {R"(l\leqslant k-1,\quad\alpha_i\geqslant0)","l⩽k−1,\u2003αᵢ⩾0"},
        {R"(x^{2}+n^{-1})","x²+n⁻¹"},
        {R"(a_{n+1}+b_{ij})","aₙ₊₁+bᵢⱼ"},
        {R"(\frac{a+b}{c})","(a+b)/c"},
        {R"(\frac{a}{bc})","a/(bc)"},
        {R"(\frac{a}{b} ^2)","(a/b) ²"},
        {R"({x+y}^{2})","(x+y)²"},
        {R"(\sqrt{x+1}+\sqrt[3]{y})","√(x+1)+∛(y)"},
        {R"(\mathbb{R}\to\mathbb{C})","ℝ→ℂ"},
        {R"(\mathfrak{ACHIRZ})","𝔄ℭℌℑℜℨ"},
        {R"(\mathbf{x}+\mathcal{F})","𝐱+ℱ"},
        {R"(\not\in A)","∉A"},
        {R"(\text{если }x\in A)","если x∈A"},
        {R"(\begin{pmatrix}a&b\\c&d\end{pmatrix})","(a\u2003b\nc\u2003d)"},
        {R"(\sum_{i=1}^{n}i^2)","∑ᵢ₌₁ⁿi²"}
    };
    for (const auto &[latex,expected]:expressions) {
        const auto converted=core::unicodeMath(latex);
        if (!converted || *converted!=expected) qFatal("Conversion %s: expected [%s], got [%s]",qPrintable(latex),qPrintable(expected),qPrintable(converted?*converted:converted.error()));
    }
    assert(!core::unicodeMath(R"(\input{secret})"));
    assert(!core::unicodeMath(R"(\frac{a}{b)"));
    assert(!core::unicodeMath(QString(17000,'x')));
    const auto inlineText=core::unicodeFormulaText(R"(справа: \(p_{3}=2\), а \(\psi\leq1\).)");
    assert(inlineText.text=="справа: p₃=2, а ψ≤1." && inlineText.converted==2 && !inlineText.unsupported);
    const auto unsupported=core::unicodeFormulaText(R"(\(x^2\), \(\unknown{x}\))");
    assert(unsupported.text==QString::fromUtf8("x², ")+R"(\(\unknown{x}\))" && unsupported.unsupported==1);
    assert(core::unicodeFormulaText("Costs $5 and $10.").text=="Costs $5 and $10.");
    core::OcrDocument book; book.sourcePath="source.png"; book.engine="fixture"; book.language="rus";
    core::OcrPage page; page.number=1; page.width=page.sourceWidth=200; page.height=page.sourceHeight=100;
    page.words.append({1,1,1,1,{10,10,100,20},R"(\(p_3=2\))",R"(\(p_3=2\))",90,true}); page.rebuildHierarchy();
    book.replacePage(page); const auto original=book.toJson(); QUndoStack undo;
    undo.push(new ui::ChangeFormulaOutputCommand(book,true));
    assert(book.unicodeFormulas && book.page(1)->words[0].text=="p₃=2");
    assert(core::formulaSourceText(book.page(1)->words[0])==R"(\(p_3=2\))");
    assert(!core::isVerifiedCorrection(book.page(1)->words[0]));
    QTemporaryDir directory; QString error;
    assert(core::ProjectFile::save(directory.filePath("unicode.yagf.json"),book,&error));
    core::OcrDocument loaded; assert(core::ProjectFile::load(directory.filePath("unicode.yagf.json"),&loaded,&error));
    assert(loaded.unicodeFormulas && loaded.page(1)->words[0].latexText==R"(\(p_3=2\))");
    auto edited=core::WordEditor::apply(*book.page(1),{0},{{"p₃=3"}}); assert(edited);
    assert(edited->words[0].latexText.isEmpty()); core::setFormulaOutput(*edited,false); assert(edited->words[0].text=="p₃=3");
    undo.undo(); assert(book.toJson()==original); undo.redo(); assert(book.page(1)->words[0].text=="p₃=2");
    core::setFormulaOutput(*book.page(1),false); assert(book.page(1)->words[0].text==R"(\(p_3=2\))");
    auto malformed=loaded.toJson(); malformed["unicode_formulas"]="yes"; assert(!core::OcrDocument::fromJson(malformed,&loaded,&error));
}

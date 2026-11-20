#include "latexdocumentexporter.h"
#include "formulatext.h"
#include "detail/pdfformulas.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
using namespace yagf;
int main(int argc,char **argv)
{
    QCoreApplication app(argc,argv); QTemporaryDir directory; assert(directory.isValid());
    core::OcrDocument book; book.sourcePath=directory.filePath("scan.png"); book.engine="fixture"; book.language="rus";
    QImage image(200,300,QImage::Format_RGB32); image.fill(Qt::white); image.setPixelColor(20,160,Qt::blue); assert(image.save(book.sourcePath));
    core::OcrPage page; page.number=1; page.width=page.sourceWidth=200; page.height=page.sourceHeight=300;
    const QStringList text{"Заголовок",R"(справа: \(p_3=2\), \(\psi\leq1\).)",R"(\[x^2=\frac{1}{2}\])", "Значение\nA\t42", "[Picture]", "Рисунок 1", R"(literal \input{ignored} # $ % & _ { } ^ ~)",R"(\end{document})"};
    for (int i=0;i<text.size();++i) {
        const auto bounds=i==4 ? QRect(20,160,70,60) : QRect(10,5+i*25,150,20);
        page.words.append({1,i+1,1,1,bounds,text[i],text[i],90}); page.words.last().layoutBlock=true;
    }
    page.rebuildHierarchy();
    const QStringList types{"heading","text","equation","table","picture","caption","text","code"};
    for (int i=0;i<types.size();++i) page.regions[i].type=types[i];
    page.regions[3].tableLayout={QJsonArray{QJsonObject{{"header",true},{"colspan",2}}},QJsonArray{QJsonObject{},QJsonObject{}}};
    core::setFormulaOutput(page,true); book.unicodeFormulas=true; book.replacePage(page);
    auto second=page; second.number=2; second.words.clear(); second.regions.clear(); second.words.append({2,1,1,1,{10,10,100,20},"Second page","Second page",90}); book.replacePage(second);
    const auto sourceSnapshot=book.toJson(); const auto destination=directory.filePath("document.tex"); std::atomic_bool cancelled=false;
    auto result=processing::LatexDocumentExporter::write(book,destination,cancelled);
    if (result.status!=processing::ExportResult::Status::Success) qFatal("%s",qPrintable(result.error));
    QFile file(destination); assert(file.open(QIODevice::ReadOnly)); const auto data=file.readAll(); file.close();
    assert(data.contains("\\section*{") && data.contains("\\frac ") && data.contains("\\psi "));
    assert(data.contains("\\multicolumn{2}") && data.contains("\\clearpage"));
    assert(!data.contains("\\input{") && data.contains("\\textbackslash{}input\\{ignored\\}"));
    const auto asset=QRegularExpression("(yagf-assets-[a-f0-9-]+/page-1-figure-5.png)").match(QString::fromUtf8(data)); assert(asset.hasMatch());
    const QImage figure(directory.filePath(asset.captured(1))); assert(figure.size()==QSize(70,60) && figure.pixelColor(0,0)==Qt::blue);
    assert(book.toJson()==sourceSnapshot);
    const auto formulas=processing::detail::collectPdfFormulas(*book.page(1),{}); assert(!formulas.formulas.isEmpty());
    bool retained=false; for (const auto &formula:formulas.formulas) retained=retained || formula.source.contains("\\frac"); assert(retained);
    const auto assetsBefore=QDir(directory.path()).entryList({"yagf-assets-*"},QDir::Dirs);
    cancelled=true; assert(processing::LatexDocumentExporter::write(book,destination,cancelled).status==processing::ExportResult::Status::Cancelled);
    cancelled=false;
    assert(processing::LatexDocumentExporter::write(book,destination,cancelled,[&](auto,auto) { cancelled=true; }).status==processing::ExportResult::Status::Cancelled);
    assert(QDir(directory.path()).entryList({"yagf-assets-*"},QDir::Dirs)==assetsBefore);
    cancelled=false; auto unsafe=book; unsafe.page(1)->words[2].text=R"(\input{secret})"; unsafe.page(1)->words[2].latexText.clear();
    assert(processing::LatexDocumentExporter::write(unsafe,destination,cancelled).status==processing::ExportResult::Status::Error);
    assert(file.open(QIODevice::ReadOnly) && file.readAll()==data); file.close();
    assert(QDir(directory.path()).entryList({"yagf-assets-*"},QDir::Dirs)==assetsBefore);
    if (!QStandardPaths::findExecutable("pdflatex").isEmpty()) {
        QProcess compiler; compiler.setWorkingDirectory(directory.path()); compiler.start("pdflatex",{"-no-shell-escape","-interaction=nonstopmode","-halt-on-error","document.tex"});
        assert(compiler.waitForFinished(90000));
        if (compiler.exitCode()) qFatal("pdfLaTeX: %s",compiler.readAllStandardOutput().constData());
        assert(QFile::exists(directory.filePath("document.pdf")));
        if (!QStandardPaths::findExecutable("pdftotext").isEmpty()) {
            compiler.start("pdftotext",{directory.filePath("document.pdf"),"-"}); assert(compiler.waitForFinished(10000) && !compiler.exitCode());
            const auto text=QString::fromUtf8(compiler.readAllStandardOutput()); assert(text.contains("Заголовок") && text.contains("Second page"));
        }
    }
}

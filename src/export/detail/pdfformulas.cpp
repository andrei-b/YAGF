#include "pdfformulas.h"
#include "exportcommand.h"
#include "formulatext.h"

#include <QFile>
#include <algorithm>
#include <expected>
#include <optional>
#include <QHash>
#include <QRegularExpression>
#include <QTextStream>

namespace yagf::processing::detail {
namespace {

std::optional<QString> delimited(QString text)
{
    text = text.trimmed();
    const auto spans = core::mathTextSpans(text);
    if (spans.size() == 1 && spans.first().length == text.size()) return spans.first().source;
    return {};
}

// Project files and OCR readings are data. Only emit math tokens, never general
// TeX primitives, file names, definitions, comments or arbitrary environments.
std::expected<QString, QString> mathTokens(const QString &source, bool allowUnicode = false)
{
    static const QSet<QString> commands = [] {
        const QString names = QStringLiteral(
            "frac dfrac tfrac binom dbinom tbinom sqrt over atop choose "
            "left right middle big Big bigg Bigg bigl bigr Bigl Bigr biggl biggr Biggl Biggr "
            "mathrm mathbf mathit mathsf mathtt mathcal mathbb mathfrak boldsymbol "
            "operatorname text textrm textnormal textbf textit mathop limits nolimits "
            "displaystyle textstyle scriptstyle scriptscriptstyle "
            "alpha beta gamma delta epsilon varepsilon zeta eta theta vartheta iota kappa lambda "
            "mu nu xi pi varpi rho varrho sigma varsigma tau upsilon phi varphi chi psi omega "
            "Gamma Delta Theta Lambda Xi Pi Sigma Upsilon Phi Psi Omega "
            "sum prod coprod int iint iiint oint lim limsup liminf max min inf sup "
            "sin cos tan cot sec csc arcsin arccos arctan sinh cosh tanh log ln exp det gcd dim "
            "pm mp times div cdot cdots ldots vdots ddots dots ast star circ bullet "
            "le leq ge geq ne neq approx sim simeq equiv cong propto ll gg not "
            "in notin ni subset supset subseteq supseteq cup cap setminus emptyset varnothing "
            "forall exists neg land lor wedge vee partial nabla infty ell hbar imath jmath "
            "to mapsto gets implies iff rightarrow leftarrow leftrightarrow Rightarrow Leftarrow "
            "Leftrightarrow longrightarrow longleftarrow longleftrightarrow uparrow downarrow "
            "langle rangle lvert rvert vert lVert rVert Vert lceil rceil lfloor rfloor "
            "hat widehat bar overline underline vec dot ddot tilde widetilde "
            "overbrace underbrace overset underset stackrel substack "
            "quad qquad enspace thinspace bmod pmod mod perp parallel mid colon "
            "oplus otimes bigcup bigcap prime Re Im aleph angle degree");
        const auto list = names.split(' ', Qt::SkipEmptyParts);
        return QSet<QString>(list.begin(), list.end());
    }();
    static const QSet<QString> environments{"matrix","pmatrix","bmatrix","Bmatrix","vmatrix","Vmatrix",
                                            "smallmatrix","aligned","gathered","cases"};
    static const QHash<QChar,QString> unicode{{u'−',"-"},{u'×',"\\times "},{u'÷',"\\div "},
        {u'≤',"\\le "},{u'≥',"\\ge "},{u'≠',"\\ne "},{u'≈',"\\approx "},{u'∞',"\\infty "},
        {u'∑',"\\sum "},{u'∫',"\\int "},{u'√',"\\sqrt "},{u'·',"\\cdot "},{u'²',"^{2}"},
        {u'°',"^{\\circ}"},{u'³',"^{3}"},{u'π',"\\pi "},{u'θ',"\\theta "},{u'α',"\\alpha "},{u'β',"\\beta "},
        {u'γ',"\\gamma "},{u'δ',"\\delta "},{u'λ',"\\lambda "},{u'μ',"\\mu "},{u'σ',"\\sigma "}};
    QString output;
    QStringList stack;
    if (source.isEmpty() || source.size() > 16384 || source.contains("^^"))
        return std::unexpected(QStringLiteral("Invalid or oversized math expression."));
    for (qsizetype i = 0; i < source.size();) {
        const QChar c = source[i++];
        if (c == '\\') {
            const auto start = i;
            while (i < source.size() && source[i].isLetter() && source[i].unicode() < 128) ++i;
            const QString name = source.mid(start, i-start);
            if (name == "begin" || name == "end") {
                while (i < source.size() && source[i].isSpace()) ++i;
                const auto end = source.indexOf('}', i);
                if (i >= source.size() || source[i] != '{' || end < 0)
                    return std::unexpected(QStringLiteral("Invalid math environment."));
                const QString env = source.mid(i+1, end-i-1);
                if (!environments.contains(env) || (name == "end" && (stack.isEmpty() || stack.takeLast() != env)))
                    return std::unexpected(QStringLiteral("Unsupported or unmatched environment %1.").arg(env));
                if (name == "begin") stack.append(env);
                output += "\\" + name + "{" + env + "}";
                i = end + 1;
            } else if (!name.isEmpty()) {
                if (!commands.contains(name)) return std::unexpected(QStringLiteral("Unsupported math command \\%1.").arg(name));
                output += "\\" + name + " ";
            } else {
                if (i == source.size() || !QStringLiteral(",;:! |{}_%#$&\\").contains(source[i]))
                    return std::unexpected(QStringLiteral("Unsupported math escape."));
                output += c;
                output += source[i++];
            }
        } else if (c == '{') { stack.append("{"); output += c; }
        else if (c == '}') {
            if (stack.isEmpty() || stack.takeLast() != "{") return std::unexpected(QStringLiteral("Unmatched math brace."));
            output += c;
        } else if (c.isSpace()) output += ' ';
        else if (unicode.contains(c)) output += unicode.value(c);
        else if (((c.unicode() < 128 || (c.unicode() >= 0x0400 && c.unicode() <= 0x04ff)) && c.isLetterOrNumber()) || QStringLiteral("+-=<>/.,:;!?()[]^_*'|&").contains(c))
            output += c;
        else if (allowUnicode && c.unicode()>=128 && (c.isPrint() || c.isHighSurrogate() || c.isLowSurrogate())) output+=c;
        else return std::unexpected(QStringLiteral("Unsupported math character %1.").arg(c));
    }
    if (!stack.isEmpty()) return std::unexpected(QStringLiteral("Unclosed math group."));
    return output;
}

QString proseTokens(QStringView source)
{
    QString text;
    for (const QChar c : source) {
        if (c == '\\') text += "\\textbackslash{}";
        else if (c == '^') text += "\\textasciicircum{}";
        else if (c == '~') text += "\\textasciitilde{}";
        else if (QStringLiteral("#$%&_{}").contains(c)) text += QStringLiteral("\\") + c;
        else if (c == '\n') text += "\\par ";
        else if (c.isSpace()) text += ' ';
        else if (c.isPrint()) text += c;
    }
    return text;
}

std::expected<QString, QString> passageTokens(const QString &source)
{
    QString text;
    qsizetype pos = 0;
    for (const auto &span : core::mathTextSpans(source)) {
        text += proseTokens(QStringView(source).mid(pos, span.start - pos));
        const bool display = span.display;
        const auto math = mathTokens(span.source.trimmed());
        if (!math) return std::unexpected(math.error());
        if (display) text += "\\par\\noindent\\hbox to \\hsize{\\hss$\\displaystyle " + *math + "$\\hss}\\par ";
        else text += "$" + *math + "$";
        pos = span.start + span.length;
    }
    text += proseTokens(QStringView(source).mid(pos));
    return text;
}

// The Qt PDF supplies the original page size and all prose. pdfTeX imports that
// page as a Form XObject, then places real TeX glyphs and rules in PDF points.
constexpr auto preamble = R"YAGF(\documentclass{article}
\usepackage{amsmath,amssymb,graphicx}
\providecommand{\degree}{^{\circ}}
\pdfminorversion=7
\pdfinfo{/Title (YAGF formatted document) /Creator (YAGF 2)}
\pdfhorigin=0pt\pdfvorigin=0pt
\newbox\basepage \newbox\outputpage \newbox\equationbox
\newcommand{\startpage}[1]{%
  \pdfximage page #1 {searchable.pdf}%
  \setbox\basepage=\hbox{\pdfrefximage\pdflastximage}%
  \pdfpagewidth=\wd\basepage \pdfpageheight=\ht\basepage
  \setbox\outputpage=\hbox to \pdfpagewidth\bgroup\rlap{\copy\basepage}\ignorespaces}
\newcommand{\putformula}[5]{%
  \setbox\equationbox=\hbox{#5}%
  \ifdim\wd\equationbox>0pt
    \setbox\equationbox=\hbox{\resizebox{#3bp}{!}{\box\equationbox}}%
    \ifdim\dimexpr\ht\equationbox+\dp\equationbox\relax>#4bp
      \setbox\equationbox=\hbox{\resizebox*{!}{#4bp}{\box\equationbox}}%
    \fi
    \dimen0=\dimexpr\pdfpageheight-#2bp-(#4bp+\ht\equationbox-\dp\equationbox)/2\relax
    \rlap{\kern#1bp\raise\dimen0\hbox to #3bp{\hss
      \pdfliteral direct{/Span << /ActualText () >> BDC}%
      \box\equationbox\pdfliteral direct{EMC}\hss}}%
  \fi\ignorespaces}
\newcommand{\finishpage}{\hss\egroup\wd\outputpage=\pdfpagewidth\ht\outputpage=\pdfpageheight
  \dp\outputpage=0pt\shipout\box\outputpage}

)YAGF";
}

QVector<FormulaSpan> editableFormulaSpans(const core::OcrPage &page, const QVector<int> &selection)
{
    QSet<QString> equations;
    for (const auto &region : page.regions) {
        if (region.type != "equation" && region.type != "formula") continue;
        for (const auto &paragraph : region.paragraphs)
            for (const auto &line : paragraph.lines)
                if (line.wordIds.size() == 1) equations.insert(line.wordIds.first());
    }
    QVector<FormulaSpan> spans;
    for (int index : selection) {
        if (index < 0 || index >= page.words.size()) continue;
        const auto &word = page.words.at(index);
        const QString original=core::formulaSourceText(word);
        const auto matches = core::mathTextSpans(original);
        if (matches.isEmpty() && equations.contains(word.id))
            spans.append({index, 0, original.size(), true});
        for (const auto &match : matches) {
            const auto delimiterLength = (match.length - match.source.size()) / 2;
            spans.append({index, match.start + delimiterLength, match.source.size(), match.display});
        }
    }
    return spans;
}

std::expected<QString, QString> safeLatexMath(const QString &source, bool allowUnicode)
{ return mathTokens(source,allowUnicode); }

std::expected<QImage, QString> renderFormulaPreview(const QString &source, bool display, const std::atomic_bool &cancelled)
{
    const auto tokens = mathTokens(source.trimmed());
    if (!tokens) return std::unexpected(tokens.error());
    QTemporaryDir dir;
    QFile tex(dir.filePath("preview.tex"));
    if (!dir.isValid() || !tex.open(QIODevice::WriteOnly))
        return std::unexpected(QStringLiteral("Could not create the preview files."));
    QTextStream out(&tex);
    out << "\\documentclass{article}\n\\usepackage{amsmath,amssymb,graphicx}\n"
           "\\providecommand{\\degree}{^{\\circ}}\n";
    if (source.contains(QRegularExpression(QStringLiteral("[\\x{0400}-\\x{04ff}]"))))
        out << "\\usepackage[T2A]{fontenc}\n";
    out << "\\begin{document}\n\\setbox0=\\hbox{$" << (display ? "\\displaystyle " : "\\textstyle ") << *tokens
        << "$}\n"
           "\\ifdim\\wd0>360pt \\setbox0=\\hbox{\\resizebox{360pt}{!}{\\box0}}\\fi\n"
           "\\ifdim\\dimexpr\\ht0+\\dp0\\relax>160pt \\setbox0=\\hbox{\\resizebox*{!}{160pt}{\\box0}}\\fi\n"
           "\\pdfhorigin=4pt\\pdfvorigin=4pt\\pdfpagewidth=\\dimexpr\\wd0+8pt\\relax\n"
           "\\pdfpageheight=\\dimexpr\\ht0+\\dp0+8pt\\relax\\shipout\\box0\\end{document}\n";
    out.flush();
    if (out.status() != QTextStream::Ok || !tex.flush()) return std::unexpected(tex.errorString());
    tex.close();
    auto compiled = runExportCommand("pdflatex", {"-no-shell-escape", "-interaction=nonstopmode",
                                      "-halt-on-error", "preview.tex"}, cancelled, dir.path());
    if (compiled.status != ExportResult::Status::Success) {
        QFile log(dir.filePath("preview.log"));
        if (log.open(QIODevice::ReadOnly)) {
            while (!log.atEnd()) {
                const auto line = log.readLine();
                if (line.startsWith("! ")) return std::unexpected(QString::fromUtf8(line.mid(2)).trimmed());
            }
        }
        return std::unexpected(compiled.error);
    }
    auto rendered = runExportCommand("pdftoppm", {"-r", "160", "-png", "-singlefile", "preview.pdf", "preview"},
                                     cancelled, dir.path());
    if (rendered.status != ExportResult::Status::Success) return std::unexpected(rendered.error);
    QImage image(dir.filePath("preview.png"));
    if (image.isNull()) return std::unexpected(QStringLiteral("Could not read the rendered formula."));
    return image;
}

PdfFormulaPage collectPdfFormulas(const core::OcrPage &page, const QRegion &protectedGraphics)
{
    PdfFormulaPage result;
    QHash<QString, const core::OcrWord *> words;
    for (const auto &word : page.words) words.insert(word.id, &word);
    for (const auto &region : page.regions) {
        if (region.type != "equation" && region.type != "formula") continue;
        for (const auto &paragraph : region.paragraphs) for (const auto &line : paragraph.lines) {
            QStringList parts, ids;
            QRect bounds;
            for (const auto &id : line.wordIds) {
                const auto *word = words.value(id);
                if (!word || word->text.trimmed().isEmpty()) continue;
                parts.append(core::formulaSourceText(*word).trimmed()); ids.append(id); bounds |= word->bounds;
            }
            if (parts.isEmpty() || protectedGraphics.intersects(bounds)) continue;
            const QString source = parts.join(' ');
            result.formulas.append({bounds, delimited(source).value_or(source), true});
            for (const auto &id : ids) result.wordIds.insert(id);
        }
    }
    for (const auto &word : page.words) {
        if (result.wordIds.contains(word.id) || protectedGraphics.intersects(word.bounds)) continue;
        const auto original=core::formulaSourceText(word);
        if (const auto source = delimited(original)) {
            result.formulas.append({word.bounds, *source, original.trimmed().startsWith("\\[") || original.trimmed().startsWith("$$")});
            result.wordIds.insert(word.id);
        } else if (!core::mathTextSpans(original).isEmpty()) {
            result.formulas.append({word.bounds, original, false, true, word.fontSizePt});
            result.wordIds.insert(word.id);
        }
    }
    return result;
}

ExportResult typesetPdfFormulas(const QVector<PdfFormulaPage> &pages, const QTemporaryDir &directory,
                               const std::atomic_bool &cancelled)
{
    QFile file(directory.filePath("typeset.tex"));
    if (!file.open(QIODevice::WriteOnly)) return {.error = file.errorString()};
    QTextStream out(&file);
    out << preamble;
    static const QRegularExpression cyrillic(QStringLiteral("[А-Яа-яЁё\\x{0400}-\\x{04ff}]"));
    const bool cyrillicFont = std::ranges::any_of(pages, [](const auto &page) {
        return std::ranges::any_of(page.formulas, [](const auto &formula) { return formula.source.contains(cyrillic); });
    });
    if (cyrillicFont) out << "\\usepackage[T2A]{fontenc}\n";
    out << "\\begin{document}\n";
    for (int i = 0; i < pages.size(); ++i) {
        out << "\\startpage{" << i+1 << "}\n";
        const auto &page = pages[i];
        for (const auto &formula : page.formulas) {
            if (cancelled.load()) return {.status = ExportResult::Status::Cancelled};
            const auto tokens = formula.passage ? passageTokens(formula.source) : mathTokens(formula.source);
            if (!tokens) return {.error = QStringLiteral("Cannot typeset the formula on page %1 at (%2, %3): %4")
                .arg(i+1).arg(formula.bounds.x()).arg(formula.bounds.y()).arg(tokens.error())};
            const QRect box = formula.bounds;
            const auto bp = [](double pixels, int dpi) { return QString::number(pixels*72.0/dpi, 'f', 4); };
            out << "\\typeout{YAGF formula on page " << i+1 << " at (" << box.x() << ", " << box.y() << ")}%\n"
                << "\\putformula{" << bp(box.x(),page.dpiX) << "}{" << bp(box.y(),page.dpiY)
                << "}{" << bp(box.width(),page.dpiX) << "}{" << bp(box.height(),page.dpiY)
                << "}{";
            if (formula.passage) {
                const int fontSize = formula.fontSizePt > 0 ? formula.fontSizePt : 10;
                // Set the paragraph at the scalable 10-point design size, then fit its box.
                out << "\\vbox{\\hsize=" << bp(box.width() * 10.0 / fontSize, page.dpiX)
                    << "bp\\parindent=0pt\\raggedright\\emergencystretch=1em\\fontsize{10}{12}\\selectfont " << *tokens << "\\par}";
            } else out << '$' << (formula.display ? "\\displaystyle " : "\\textstyle ") << *tokens << '$';
            out << "}\n";
        }
        out << "\\finishpage\n";
    }
    out << "\\end{document}\n";
    out.flush();
    if (out.status() != QTextStream::Ok || !file.flush()) return {.error = file.errorString()};
    file.close();
    auto result = runExportCommand("pdflatex", {"-no-shell-escape", "-interaction=nonstopmode",
        "-halt-on-error", "typeset.tex"}, cancelled, directory.path());
    if (result.status == ExportResult::Status::Error) {
        QFile log(directory.filePath("typeset.log"));
        if (log.open(QIODevice::ReadOnly)) {
            const QString data = QString::fromUtf8(log.readAll());
            const auto marker = data.lastIndexOf("YAGF formula on page ");
            const auto reason = data.indexOf("\n! ");
            if (reason >= 0) result.error = QStringLiteral("PDF typesetting failed. %1 %2")
                .arg(marker >= 0 ? data.mid(marker).section('\n',0,0) : QString(), data.mid(reason+3).section('\n',0,0));
        }
    }
    return result;
}
}

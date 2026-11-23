#include "formulatext.h"
#include "ocrdocument.h"
#include <QHash>
#include <QSet>
#include <QRegularExpression>

namespace yagf::core {
namespace {
const QHash<QString, QString> &symbols()
{
    static const auto values = [] {
        QHash<QString,QString> m;
        const QString names = "alpha beta gamma delta epsilon varepsilon zeta eta theta vartheta iota kappa varkappa lambda mu nu xi omicron pi varpi rho varrho sigma varsigma tau upsilon phi varphi chi psi omega Gamma Delta Theta Lambda Xi Pi Sigma Upsilon Phi Psi Omega";
        const QString letters = QString::fromUtf8("α β γ δ ϵ ε ζ η θ ϑ ι κ ϰ λ μ ν ξ ο π ϖ ρ ϱ σ ς τ υ ϕ φ χ ψ ω Γ Δ Θ Λ Ξ Π Σ Υ Φ Ψ Ω");
        const auto a=names.split(' '), b=letters.split(' ');
        for (int i=0;i<a.size();++i) m.insert(a[i],b[i]);
        const QVector<QPair<QString,QString>> operators{
            {"pm","±"},{"mp","∓"},{"times","×"},{"div","÷"},{"cdot","·"},{"ast","∗"},{"star","⋆"},{"circ","∘"},{"bullet","∙"},
            {"le","≤"},{"leq","≤"},{"ge","≥"},{"geq","≥"},{"ne","≠"},{"neq","≠"},{"approx","≈"},{"sim","∼"},{"simeq","≃"},{"equiv","≡"},{"cong","≅"},{"propto","∝"},{"ll","≪"},{"gg","≫"},
            {"in","∈"},{"notin","∉"},{"ni","∋"},{"subset","⊂"},{"supset","⊃"},{"subseteq","⊆"},{"supseteq","⊇"},{"cup","∪"},{"cap","∩"},{"setminus","∖"},{"emptyset","∅"},{"varnothing","∅"},
            {"forall","∀"},{"exists","∃"},{"nexists","∄"},{"neg","¬"},{"land","∧"},{"wedge","∧"},{"lor","∨"},{"vee","∨"},{"partial","∂"},{"nabla","∇"},{"infty","∞"},{"ell","ℓ"},{"hbar","ℏ"},{"imath","ı"},{"jmath","ȷ"},
            {"sum","∑"},{"prod","∏"},{"coprod","∐"},{"int","∫"},{"iint","∬"},{"iiint","∭"},{"oint","∮"},{"bigcup","⋃"},{"bigcap","⋂"},
            {"to","→"},{"mapsto","↦"},{"gets","←"},{"rightarrow","→"},{"leftarrow","←"},{"leftrightarrow","↔"},{"Rightarrow","⇒"},{"Leftarrow","⇐"},{"Leftrightarrow","⇔"},{"implies","⇒"},{"iff","⇔"},
            {"longrightarrow","⟶"},{"longleftarrow","⟵"},{"longleftrightarrow","⟷"},{"uparrow","↑"},{"downarrow","↓"},
            {"langle","⟨"},{"rangle","⟩"},{"lvert","|"},{"rvert","|"},{"vert","|"},{"lVert","‖"},{"rVert","‖"},{"Vert","‖"},{"lceil","⌈"},{"rceil","⌉"},{"lfloor","⌊"},{"rfloor","⌋"},
            {"perp","⊥"},{"parallel","∥"},{"mid","∣"},{"colon",":"},{"oplus","⊕"},{"otimes","⊗"},{"prime","′"},{"Re","ℜ"},{"Im","ℑ"},{"aleph","ℵ"},{"angle","∠"},{"degree","°"},
            {"dots","…"},{"ldots","…"},{"cdots","⋯"},{"vdots","⋮"},{"ddots","⋱"},{"quad","\u2003"},{"qquad","\u2003\u2003"},{"enspace","\u2002"},{"thinspace","\u2009"},
            {"bmod"," mod "},{"mod"," mod "}};
        for (const auto &p:operators) m.insert(p.first,p.second);
        return m;
    }();
    return values;
}
QString script(const QString &text,bool upper)
{
    const QString plain=upper ? QStringLiteral("0123456789+-=()in") : QStringLiteral("0123456789+-=()aehi jklmnoprstuvxβγρφχ").remove(' ');
    const QString glyphs=upper ? QString::fromUtf8("⁰¹²³⁴⁵⁶⁷⁸⁹⁺⁻⁼⁽⁾ⁱⁿ") : QString::fromUtf8("₀₁₂₃₄₅₆₇₈₉₊₋₌₍₎ₐₑₕᵢⱼₖₗₘₙₒₚᵣₛₜᵤᵥₓᵦᵧᵨᵩᵪ");
    QString out;
    for (QChar c:text) { const int i=plain.indexOf(c==QChar(0x2212)?QChar('-'):c); if (i<0) return QString(upper?'^':'_')+'('+text+')'; out+=glyphs[i]; }
    return out;
}
QString styled(const QString &text,const QString &style)
{
    static const QHash<QChar,uint> fraktur{{'C',0x212d},{'H',0x210c},{'I',0x2111},{'R',0x211c},{'Z',0x2128}};
    static const QHash<QChar,uint> doubleStruck{{'C',0x2102},{'H',0x210d},{'N',0x2115},{'P',0x2119},{'Q',0x211a},{'R',0x211d},{'Z',0x2124}};
    static const QHash<QChar,uint> calligraphic{{'B',0x212c},{'E',0x2130},{'F',0x2131},{'H',0x210b},{'I',0x2110},{'L',0x2112},{'M',0x2133},{'R',0x211b},{'e',0x212f},{'g',0x210a},{'o',0x2134}};
    QString out;
    for (const QChar c:text) {
        char32_t code=c.unicode();
        if (c>='A' && c<='Z') {
            if (style=="mathfrak") code=fraktur.value(c,0x1d504+c.unicode()-'A');
            else if (style=="mathbb") code=doubleStruck.value(c,0x1d538+c.unicode()-'A');
            else if (style=="mathcal") code=calligraphic.value(c,0x1d49c+c.unicode()-'A');
            else if (style=="mathbf" || style=="boldsymbol") code=0x1d400+c.unicode()-'A';
            else if (style=="mathit") code=0x1d434+c.unicode()-'A';
        } else if (c>='a' && c<='z') {
            if (style=="mathfrak") code=0x1d51e + c.unicode()-'a';
            else if (style=="mathbb") code=0x1d552+c.unicode()-'a';
            else if (style=="mathcal") code=calligraphic.value(c,0x1d4b6+c.unicode()-'a');
            else if (style=="mathbf" || style=="boldsymbol") code=0x1d41a+c.unicode()-'a';
            else if (style=="mathit") code=c=='h'?0x210e:0x1d44e + c.unicode()-'a';
        } else if (c>='0' && c<='9') {
            if (style=="mathbb") code=0x1d7d8+c.unicode()-'0';
            else if (style=="mathbf" || style=="boldsymbol") code=0x1d7ce + c.unicode()-'0';
        }
        if (style=="boldsymbol" || style=="mathbf") {
            const QString upper=QString::fromUtf8("ΑΒΓΔΕΖΗΘΙΚΛΜΝΞΟΠΡϴΣΤΥΦΧΨΩ"), lower=QString::fromUtf8("αβγδεζηθικλμνξοπρςστυφχψω");
            if (upper.contains(c)) code=0x1d6a8+upper.indexOf(c);
            else if (lower.contains(c)) code=0x1d6c2+lower.indexOf(c);
        }
        out+=QString::fromUcs4(&code,1);
    }
    return out;
}
class Parser {
public:
    explicit Parser(QString source):source(std::move(source)){}
    std::expected<QString,QString> run()
    {
        if (source.isEmpty() || source.size()>16384) return std::unexpected(QStringLiteral("Empty or oversized expression."));
        auto text=sequence();
        if (!error.isEmpty()) return std::unexpected(error);
        return text.trimmed();
    }
private:
    QString source,error;
    qsizetype position=0;
    int depth=0;
    void spaces() { while (position<source.size() && source[position].isSpace()) ++position; }
    QChar following() const { auto p=position; while (p<source.size() && source[p].isSpace()) ++p; return p<source.size()?source[p]:QChar{}; }
    QString sequence(QChar end={})
    {
        if (++depth>64) { error="Expression is nested too deeply."; --depth; return {}; }
        QString out;
        while (position<source.size() && error.isEmpty()) {
            if (!end.isNull() && source[position]==end) { ++position; --depth; return out; }
            if (source[position]=='}' || source[position]==']' && end==']') { error="Unmatched math group."; break; }
            const auto before=position; out+=atom();
            if (position==before) { error="Invalid math token."; break; }
        }
        if (!end.isNull() && error.isEmpty()) error="Unclosed math group.";
        --depth; return out;
    }
    QString argument()
    {
        spaces();
        if (position>=source.size()) { error="Missing math argument."; return {}; }
        if (source[position]=='{') { ++position; return sequence('}'); }
        return atom();
    }
    QString literal()
    {
        spaces(); if (position>=source.size() || source[position]!='{') { error="Missing text argument."; return {}; }
        ++position; int nesting=1; QString out;
        while (position<source.size()) {
            const auto c=source[position++];
            if (c=='{') { ++nesting; out+=c; }
            else if (c=='}') { if (!--nesting) return out; out+=c; }
            else if (c=='\\' && position<source.size()) {
                if (QStringLiteral("{}%&#_$\\").contains(source[position])) out+=source[position++];
                else { --position; out+=atom(); }
            } else out+=c;
        }
        error="Unclosed text argument."; return {};
    }
    QString environment()
    {
        const QString env=literal();
        static const QSet<QString> supported{"matrix","pmatrix","bmatrix","Bmatrix","vmatrix","Vmatrix","smallmatrix","aligned","gathered","cases"};
        if (!supported.contains(env)) { error="Unsupported math environment: "+env; return {}; }
        const QString ending="\\end{"+env+'}';
        const auto end=source.indexOf(ending,position);
        if (end<0) { error="Unclosed math environment."; return {}; }
        QString body=source.mid(position,end-position); position=end+ending.size();
        Parser nested(body); nested.depth=depth; auto converted=nested.run();
        if (!converted) { error=converted.error(); return {}; }
        const auto text=*converted;
        if (env=="pmatrix") return '('+text+')';
        if (env=="bmatrix" || env=="matrix" || env=="smallmatrix") return '['+text+']';
        if (env=="Bmatrix" || env=="cases") return '{'+text+'}';
        if (env=="vmatrix") return '|'+text+'|';
        if (env=="Vmatrix") return QStringLiteral("‖")+text+QStringLiteral("‖");
        return text;
    }
    QString atom()
    {
        if (position>=source.size()) return {};
        const QChar c=source[position++];
        if (c=='{') {
            const auto group=sequence('}');
            if (group.toUcs4().size()>1 && (following()=='^' || following()=='_')) return '('+group+')';
            return group;
        }
        if (c=='^' || c=='_') return script(argument(),c=='^');
        if (c=='&') return QStringLiteral("\u2003");
        if (c=='~') return " ";
        if (c=='%') { error="Unsupported math comment."; return {}; }
        if (c!='\\') return c=='-' ? QStringLiteral("−") : QString(c);
        const auto start=position;
        while (position<source.size() && source[position].unicode()<128 && source[position].isLetter()) ++position;
        const QString name=source.mid(start,position-start);
        if (name.isEmpty()) {
            if (position>=source.size()) { error="Incomplete math command."; return {}; }
            const QChar escaped=source[position++];
            if (escaped=='\\') return "\n";
            if (QStringLiteral(",;: ").contains(escaped)) return QStringLiteral("\u2009");
            if (escaped=='!') return {};
            if (QStringLiteral("{}|_%#$&").contains(escaped)) return QString(escaped);
            error="Unsupported math escape."; return {};
        }
        // A TeX control word consumes its following delimiter whitespace.
        spaces();
        if (name=="begin") return environment();
        if (name=="left" || name=="right" || name=="middle") {
            if (position<source.size() && source[position]=='.') ++position;
            return {};
        }
        static const QSet<QString> ignored{"displaystyle","textstyle","scriptstyle","scriptscriptstyle","limits","nolimits","big","Big","bigg","Bigg","bigl","bigr","Bigl","Bigr","biggl","biggr","Biggl","Biggr"};
        if (ignored.contains(name)) return {};
        if (symbols().contains(name)) return symbols().value(name);
        static const QSet<QString> functions{"sin","cos","tan","cot","sec","csc","arcsin","arccos","arctan","sinh","cosh","tanh","log","ln","exp","det","gcd","dim","lim","limsup","liminf","max","min","inf","sup","Pr"};
        if (functions.contains(name)) return name+' ';
        if (name=="frac" || name=="dfrac" || name=="tfrac") {
            const auto a=argument(),b=argument();
            const auto group=[](QString t) { return t.contains(QRegularExpression("[+−=/< >]")) ? '('+t+')' : t; };
            const bool compound=b.toUcs4().size()>1 && !QRegularExpression("^[0-9]+(?:\\.[0-9]+)?$").match(b).hasMatch();
            const auto denominator=compound && !(b.startsWith('(') && b.endsWith(')')) ? '('+b+')' : group(b);
            auto fraction=group(a)+'/'+denominator;
            const auto next=following();
            if (next=='^' || next=='_' || next.isLetterOrNumber() || next=='\\' || next=='{') fraction='('+fraction+')';
            return fraction;
        }
        if (name=="binom" || name=="dbinom" || name=="tbinom") { const auto a=argument(),b=argument(); return "binom("+a+", "+b+')'; }
        if (name=="sqrt") {
            QString index;
            if (position<source.size() && source[position]=='[') { ++position; index=sequence(']'); }
            const auto body=argument();
            const QString radical=index=="3" ? "∛" : index=="4" ? "∜" : index.isEmpty() || index=="2" ? "√" : script(index,true)+QStringLiteral("√");
            return radical+'('+body+')';
        }
        if (name=="text" || name=="textrm" || name=="textnormal" || name=="textbf" || name=="textit" || name=="operatorname") return literal();
        if (name=="mathrm" || name=="mathsf" || name=="mathtt" || name=="mathop") return argument();
        if (name=="mathfrak" || name=="mathbb" || name=="mathcal" || name=="mathbf" || name=="boldsymbol" || name=="mathit") return styled(argument(),name);
        if (name=="pmod") return " (mod "+argument()+')';
        if (name=="not") {
            const auto target=argument();
            static const QHash<QString,QString> negated{{"=","≠"},{"∈","∉"},{"⊂","⊄"},{"⊆","⊈"},{"≤","≰"},{"≥","≱"}};
            return negated.value(target,target+QStringLiteral("\u0338"));
        }
        static const QHash<QString,char16_t> accents{{"hat",0x0302},{"widehat",0x0302},{"bar",0x0305},{"overline",0x0305},{"underline",0x0332},{"vec",0x20d7},{"dot",0x0307},{"ddot",0x0308},{"tilde",0x0303},{"widetilde",0x0303}};
        if (accents.contains(name)) {
            QString out; for (const auto code:argument().toUcs4()) { const char32_t value=code; out+=QString::fromUcs4(&value,1); if (!QChar::isSpace(value)) out+=accents.value(name); } return out;
        }
        if (name=="overset" || name=="stackrel" || name=="underset") { const auto annotation=argument(),body=argument(); return body+script(annotation,name!="underset"); }
        if (name=="substack") return '('+argument().replace('\n',';')+')';
        error="Unsupported math command: \\"+name; return {};
    }
};
bool escaped(const QString &text,qsizetype at)
{
    int count=0; while (at>0 && text[--at]=='\\') ++count; return count%2;
}
}

QVector<SymbolChoice> localSymbolChoices(const core::OcrPage &page, int index, int start, int length)
{
    if (index < 0 || index >= page.words.size()) return {};
    const auto &target = page.words[index];
    const auto text = formulaSourceText(target);
    if (length != 1 || start < 0 || start >= text.size()) return {};
    const auto inWord = [&](int at) { return at >= 0 && at < text.size() && text[at].isLetter(); };
    if (inWord(start-1) || inWord(start+1)) return {};
    QString before = text.left(start).trimmed(), after = text.mid(start+1).trimmed();
    const auto sameParagraph = [&](int other) {
        return other >= 0 && other < page.words.size() && target.block == page.words[other].block &&
               target.paragraph == page.words[other].paragraph;
    };
    if (before.isEmpty() && sameParagraph(index-1)) before = formulaSourceText(page.words[index-1]).trimmed();
    if (after.isEmpty() && sameParagraph(index+1)) after = formulaSourceText(page.words[index+1]).trimmed();
    const QString operators = "=<>≤≥±+−*/^_";
    const bool mathematicalRole = (!before.isEmpty() && operators.contains(before.back())) ||
                                  (!after.isEmpty() && operators.contains(after.front()));
    static const QRegularExpression russianNoun(QStringLiteral(R"(^[А-Яа-яЁё]{2,})"));
    const bool preposition = QStringLiteral("cс").contains(text[start]) && russianNoun.match(after).hasMatch();
    const QStringList groups = {"cсϲ","xхχ","pрρ","aаα","oоο","vν","uυ","yуγ","BВΒ","HНΗ","KКΚ","TТΤ"};
    QVector<SymbolChoice> choices;
    for (const auto &group : groups) {
        if (!group.contains(text[start])) continue;
        for (QChar letter : group) {
            int score = letter == text[start] ? 1 : 0;
            if (letter.script() == QChar::Script_Latin && mathematicalRole) score += 3;
            if (letter.script() == QChar::Script_Cyrillic && preposition) score += 6;
            for (int n=0; n<page.words.size(); ++n) {
                const auto &word = page.words[n];
                if (word.block != target.block || word.paragraph != target.paragraph) continue;
                const auto context = formulaSourceText(word);
                for (int i=0; i<context.size(); ++i) {
                    if (context[i] != letter || (n==index && i==start)) continue;
                    if ((i && context[i-1].isLetter()) ||
                        (i+1<context.size() && context[i+1].isLetter())) continue;
                    // Count each occurrence once, giving confirmed notation and
                    // identifiers beside mathematical operators greater weight.
                    int left = i-1, right = i+1;
                    while (left >= 0 && context[left].isSpace()) --left;
                    while (right < context.size() && context[right].isSpace()) ++right;
                    const bool math = (left >= 0 && operators.contains(context[left])) ||
                                      (right < context.size() && operators.contains(context[right]));
                    score += (word.reviewed ? 3 : 1) + (math ? 2 : 0);
                }
            }
            choices.append({QString(letter), score});
        }
        break;
    }
    std::stable_sort(choices.begin(), choices.end(), [](const auto &a, const auto &b) {
        return a.evidence > b.evidence;
    });
    return choices;
}

QVector<MathTextSpan> mathTextSpans(const QString &text)
{
    QVector<MathTextSpan> spans;
    for (qsizetype i=0;i<text.size();) {
        QString left,right; bool display=false;
        if (text.mid(i,2)=="\\(" && !escaped(text,i)) { left="\\("; right="\\)"; }
        else if (text.mid(i,2)=="\\[" && !escaped(text,i)) { left="\\["; right="\\]"; display=true; }
        else if (text.mid(i,2)=="$$" && !escaped(text,i)) { left=right="$$"; display=true; }
        else if (text[i]=='$' && !escaped(text,i)) left=right="$";
        if (left.isEmpty()) { ++i; continue; }
        qsizetype end=text.indexOf(right,i+left.size());
        while (end>=0 && escaped(text,end)) end=text.indexOf(right,end+right.size());
        if (end<0 || (left=="$" && end+1<text.size() && text[end+1].isDigit())) { i+=left.size(); continue; }
        if (end>i+left.size()) spans.append({i,end+right.size()-i,text.mid(i+left.size(),end-i-left.size()),display});
        i=end+right.size();
    }
    return spans;
}
std::expected<QString,QString> unicodeMath(const QString &latex) { return Parser(latex).run(); }
QString latexForUnicode(char32_t symbol)
{
    static const auto mapping=[] {
        QHash<char32_t,QString> map;
        auto names=symbols().keys(); names.sort();
        for (const auto &name:names) {
            const auto points=symbols().value(name).toUcs4();
            if (points.size()==1 && points[0]>=128 && !QChar::isSpace(points[0]) && !map.contains(points[0])) map.insert(points[0],"\\"+name+' ');
        }
        map.insert(0x2212,"-");
        for (bool upper:{false,true}) {
            const auto text=QStringLiteral("0123456789+-=()in");
            for (const auto c:text) {
                const auto converted=script(QString(c),upper).toUcs4();
                if (converted.size()==1 && converted[0]>=128) map.insert(converted[0],QString(upper?"{}^{":"{}_{")+c+'}');
            }
        }
        for (const auto c:QStringLiteral("aehjklmoprstuvx")) {
            const auto points=script(QString(c),false).toUcs4();
            if (points.size()==1) map.insert(points[0],QStringLiteral("{}_{")+c+'}');
        }
        for (const auto &style:QStringList{"mathbb","mathcal","mathbf","mathit","mathfrak"})
            for (const auto c:QStringLiteral("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789")) {
                const auto points=styled(QString(c),style).toUcs4();
                if (points.size()==1 && points[0]!=c.unicode() && (style!="mathfrak" || !map.contains(points[0]))) map.insert(points[0],"\\"+style+'{'+c+'}');
            }
        return map;
    }();
    return mapping.value(symbol);
}
UnicodeFormulaText unicodeFormulaText(const QString &text,bool wholeFormula)
{
    UnicodeFormulaText result;
    const auto spans=mathTextSpans(text);
    if (spans.isEmpty()) {
        result.text=text;
        if (wholeFormula) { auto converted=unicodeMath(text); if (converted) { result.text=*converted; result.converted=1; } else result.unsupported=1; }
        return result;
    }
    qsizetype position=0;
    for (const auto &span:spans) {
        result.text+=text.mid(position,span.start-position);
        const auto converted=unicodeMath(span.source);
        if (converted) { result.text+=*converted; ++result.converted; }
        else { result.text+=text.mid(span.start,span.length); ++result.unsupported; }
        position=span.start+span.length;
    }
    result.text+=text.mid(position); return result;
}
QString formulaSourceText(const OcrWord &word)
{
    if (!word.latexText.isEmpty() && unicodeFormulaText(word.latexText,true).text==word.text) return word.latexText;
    return word.text;
}
bool isFormulaWord(const OcrPage &page,const OcrWord &word)
{
    for (const auto &region:page.regions) if (region.type=="equation" || region.type=="formula")
        for (const auto &paragraph:region.paragraphs) for (const auto &line:paragraph.lines)
            if (line.wordIds.contains(word.id)) return true;
    return page.engineId=="rapid-latex-ocr";
}
int setFormulaOutput(OcrPage &page,bool unicode)
{
    int unsupported=0;
    const auto convert=[&](OcrWord &word) {
        const auto source=formulaSourceText(word);
        if (!unicode) { word.text=source; word.latexText.clear(); return; }
        const auto result=unicodeFormulaText(source,isFormulaWord(page,word));
        unsupported+=result.unsupported;
        word.text=result.text; word.latexText=source!=word.text ? source : QString();
    };
    for (auto &word:page.words) convert(word);
    for (auto &word:page.pendingCorrections) convert(word);
    for (auto &word:page.deletedWords) convert(word);
    return unsupported;
}
} // namespace yagf::core

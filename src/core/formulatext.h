#pragma once
#include <QString>
#include <QVector>
#include <expected>

namespace yagf::core {
struct OcrWord;
struct OcrPage;
struct MathTextSpan { qsizetype start, length; QString source; bool display = false; };
QVector<MathTextSpan> mathTextSpans(const QString &text);
std::expected<QString, QString> unicodeMath(const QString &latex);
QString latexForUnicode(char32_t symbol);
struct UnicodeFormulaText { QString text; int converted = 0, unsupported = 0; };
UnicodeFormulaText unicodeFormulaText(const QString &text, bool wholeFormula = false);
QString formulaSourceText(const OcrWord &word);
bool isFormulaWord(const OcrPage &page, const OcrWord &word);
int setFormulaOutput(OcrPage &page, bool unicode);
} // namespace yagf::core

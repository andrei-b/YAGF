#include "spellhighlighter.h"
#include <QColor>
#include <QRegularExpression>
#include <QTextCharFormat>
namespace yagf::ui {
SpellHighlighter::SpellHighlighter(QTextDocument *document) : QSyntaxHighlighter(document)
{ setObjectName(QStringLiteral("spellHighlighter")); }
SpellHighlighter::~SpellHighlighter() = default;
QStringList SpellHighlighter::availableDictionaries() const { return checker.availableDictionaries(); }
QStringList SpellHighlighter::activeDictionaries() const { return checker.activeDictionaries(); }
QString SpellHighlighter::statusText() const { return enabled ? checker.statusText() : tr("Spell check is off."); }
void SpellHighlighter::setLanguages(const QStringList &languages) { checker.setLanguages(languages); rehighlight(); }
void SpellHighlighter::setEnabled(bool value) { enabled = value; rehighlight(); }
bool SpellHighlighter::isMisspelled(const QString &word) { return enabled && checker.isMisspelled(word); }
void SpellHighlighter::highlightBlock(const QString &text)
{
    if (!enabled || checker.activeDictionaries().isEmpty()) return;
    static const QRegularExpression words(QStringLiteral("[\\p{L}\\p{M}\\p{N}_]+(?:['’][\\p{L}\\p{M}]+)*"));
    static const QRegularExpression identifiers(QStringLiteral("[\\p{N}_]"));
    QTextCharFormat underline;
    underline.setUnderlineStyle(QTextCharFormat::SpellCheckUnderline);
    underline.setUnderlineColor(QColor(210, 45, 55));
    auto matches = words.globalMatch(text);
    while (matches.hasNext()) {
        const auto match = matches.next();
        if (!match.captured().contains(identifiers) && isMisspelled(match.captured()))
            setFormat(match.capturedStart(), match.capturedLength(), underline);
    }
}

} // namespace yagf::ui

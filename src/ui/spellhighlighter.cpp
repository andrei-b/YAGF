#include "spellhighlighter.h"
#include <QColor>
#include <QAbstractTextDocumentLayout>
#include <QContextMenuEvent>
#include <QMenu>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextEdit>
#include <QRegularExpression>
#include <QTextCharFormat>
namespace yagf::ui {
SpellHighlighter::SpellHighlighter(QTextEdit *editor)
    : QSyntaxHighlighter(editor->document()), editor(editor)
{
    setObjectName(QStringLiteral("spellHighlighter"));
    editor->viewport()->installEventFilter(this);
}
SpellHighlighter::~SpellHighlighter() = default;
QStringList SpellHighlighter::availableDictionaries() const { return checker.availableDictionaries(); }
QStringList SpellHighlighter::activeDictionaries() const { return checker.activeDictionaries(); }
QString SpellHighlighter::statusText() const { return enabled ? checker.statusText() : tr("Spell check is off."); }
void SpellHighlighter::setLanguages(const QStringList &languages) { checker.setLanguages(languages); rehighlight(); }
void SpellHighlighter::setEnabled(bool value) { enabled = value; rehighlight(); }
bool SpellHighlighter::isMisspelled(const QString &word) { return enabled && checker.isMisspelled(word); }
bool SpellHighlighter::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() != QEvent::ContextMenu || !enabled || editor->isReadOnly()) return false;
    auto *context = static_cast<QContextMenuEvent *>(event);
    const bool keyboard = context->reason() == QContextMenuEvent::Keyboard;
    const QPoint viewPoint = keyboard ? editor->cursorRect().center() : context->pos();
    const QPoint documentPoint = viewPoint + QPoint(editor->horizontalScrollBar()->value(),
                                                    editor->verticalScrollBar()->value());
    const int position = keyboard ? editor->textCursor().position()
        : document()->documentLayout()->hitTest(documentPoint, Qt::ExactHit);
    if (position < 0) return false;
    const QTextBlock block = document()->findBlock(position);
    if (!block.isValid()) return false;
    for (const auto &range : block.layout()->formats()) {
        const int start = block.position() + range.start, end = start + range.length;
        if (range.format.underlineStyle() != QTextCharFormat::SpellCheckUnderline ||
            position < start || position > end || (!keyboard && position == end)) continue;
        QTextCursor target(document());
        target.setPosition(start);
        target.setPosition(end, QTextCursor::KeepAnchor);
        const QString word = target.selectedText();
        const int revision = document()->revision();
        std::unique_ptr<QMenu> menu(editor->createStandardContextMenu(documentPoint));
        QAction *before = menu->actions().value(0);
        menu->insertSection(before, tr("Spelling suggestions"));
        const auto replacements = checker.suggestions(word);
        for (const auto &replacement : replacements) {
            auto *action = new QAction(QString(replacement).replace('&', "&&"), menu.get());
            menu->insertAction(before, action);
            connect(action, &QAction::triggered, this, [this, target, revision, replacement]() mutable {
                if (editor->isReadOnly() || document()->revision() != revision) return;
                QTextCursor first(target);
                first.setPosition(target.selectionStart() + 1);
                target.insertText(replacement, first.charFormat());
                editor->setTextCursor(target);
            });
        }
        if (replacements.isEmpty()) {
            auto *empty = new QAction(tr("No suggestions"), menu.get());
            empty->setEnabled(false);
            menu->insertAction(before, empty);
        }
        menu->insertSeparator(before);
        if (!editor->actions().isEmpty()) {
            menu->addSeparator();
            menu->addActions(editor->actions());
        }
        menu->exec(editor->viewport()->mapToGlobal(viewPoint));
        event->accept();
        return true;
    }
    return QSyntaxHighlighter::eventFilter(watched, event);
}
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

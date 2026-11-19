#pragma once

#include "spellchecker.h"
#include <QStringList>
#include <QSyntaxHighlighter>

#include <memory>

class QTextEdit;

namespace yagf::ui {

// Extra layout formats underline spelling errors without editing OCR or rich-text content.
class SpellHighlighter final : public QSyntaxHighlighter {
public:
    explicit SpellHighlighter(QTextEdit *editor);
    ~SpellHighlighter() override;
    QStringList availableDictionaries() const;
    QStringList activeDictionaries() const;
    QString statusText() const;
    void setLanguages(const QStringList &languages);
    void setEnabled(bool enabled);
    bool isMisspelled(const QString &word);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void highlightBlock(const QString &text) override;

private:
    QTextEdit *editor;
    core::SpellChecker checker;
    bool enabled = true;
};

} // namespace yagf::ui

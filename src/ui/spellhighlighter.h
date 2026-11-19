#pragma once

#include "spellchecker.h"
#include <QStringList>
#include <QSyntaxHighlighter>

#include <memory>

namespace yagf::ui {

// Extra layout formats underline spelling errors without editing OCR or rich-text content.
class SpellHighlighter final : public QSyntaxHighlighter {
public:
    explicit SpellHighlighter(QTextDocument *document);
    ~SpellHighlighter() override;
    QStringList availableDictionaries() const;
    QStringList activeDictionaries() const;
    QString statusText() const;
    void setLanguages(const QStringList &languages);
    void setEnabled(bool enabled);
    bool isMisspelled(const QString &word);

protected:
    void highlightBlock(const QString &text) override;

private:
    core::SpellChecker checker;
    bool enabled = true;
};

} // namespace yagf::ui

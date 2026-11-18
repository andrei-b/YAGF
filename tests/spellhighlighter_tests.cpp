#include "spellhighlighter.h"

#include <QApplication>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QTextLayout>
#include <cstdio>
#include <algorithm>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

using yagf::ui::SpellHighlighter;

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QTextEdit editor;
    editor.show();
    QApplication::processEvents();
    QTextDocument &document = *editor.document();
    document.setHtml("<p><b>Hello</b> qzxqzxqzx world 12345 widget_42 QWidget2</p>");
    document.setUndoRedoEnabled(false);
    document.setUndoRedoEnabled(true);
    document.setModified(false);
    SpellHighlighter spelling(&document);
    spelling.setLanguages({"zz_ZZ"});
    assert(spelling.activeDictionaries().isEmpty());
    assert(!spelling.isMisspelled("anything"));
    if (spelling.availableDictionaries().isEmpty()) {
        std::puts("No Enchant dictionaries installed; checked unavailable-dictionary behavior.");
        return 0;
    }
    assert(spelling.statusText().contains("zz_ZZ"));
    const auto available = spelling.availableDictionaries();
    const bool english = std::ranges::any_of(available, [](const QString &code) { return code == "en" || code.startsWith("en_"); });
    if (!english) {
        std::puts("No English dictionary; checked installed dictionaries and unavailable-language behavior.");
        return 0;
    }
    const QString originalHtml = document.toHtml(), originalText = document.toPlainText();
    spelling.setLanguages({"eng"});
    assert(!spelling.activeDictionaries().isEmpty());
    assert(!spelling.isMisspelled("Hello"));
    assert(!spelling.isMisspelled("world"));
    assert(spelling.isMisspelled("qzxqzxqzx"));
    assert(!spelling.isMisspelled("don't"));
    assert(!spelling.isMisspelled(QString::fromUtf8("don’t")));
    const auto underlines = [&] {
        QVector<QTextLayout::FormatRange> found;
        for (auto block = document.begin(); block.isValid(); block = block.next())
            for (const auto &range : block.layout()->formats())
                if (range.format.underlineStyle() == QTextCharFormat::SpellCheckUnderline) found.append(range);
        return found;
    };
    spelling.rehighlight();
    assert(underlines().size() == 1 && underlines()[0].start == 6 && underlines()[0].length == 9);
    assert(document.toHtml() == originalHtml && !document.isModified() && !document.isUndoAvailable());
    QTextCursor cursor(&document);
    cursor.setPosition(1);
    assert(cursor.charFormat().fontWeight() >= QFont::Bold);
    spelling.setEnabled(false);
    assert(underlines().isEmpty() && document.toHtml() == originalHtml && !document.isModified());
    spelling.setEnabled(true);
    assert(underlines().size() == 1);

    cursor.setPosition(6);
    cursor.setPosition(15, QTextCursor::KeepAnchor);
    cursor.insertText("friend");
    QApplication::processEvents();
    assert(underlines().isEmpty());
    document.undo();
    QApplication::processEvents();
    assert(document.toPlainText() == originalText && underlines().size() == 1);
    assert(!document.isUndoAvailable());

    if (std::ranges::any_of(available, [](const QString &code) { return code.startsWith("de_"); })) {
        spelling.setLanguages({"eng+deu"});
        assert(spelling.activeDictionaries().size() == 2);
        assert(!spelling.isMisspelled("Hello") && !spelling.isMisspelled(QString::fromUtf8("Straße")));
        assert(spelling.isMisspelled("qzxqzxqzx"));
        spelling.setLanguages({"deu"});
        assert(spelling.activeDictionaries() == QStringList{"de_DE"});
    }
    spelling.setLanguages({"eng+zz_ZZ"});
    assert(spelling.activeDictionaries().isEmpty() && underlines().isEmpty());
}

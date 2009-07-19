#include <QTextEdit>
#include <QRegExp>
#include <QTextCursor>
#include <QTextCharFormat>
#include "spellchecker.h"


SpellChecker::SpellChecker(QTextEdit * textEdit):m_textEdit(textEdit),m_lang1("ru"), m_lang2("en")
{
    m_regExp = new QRegExp("[^\\s]*");
    //m_cursor= new QTextCursor(m_textEdit->document());
    spell_config1 = new_aspell_config();
    spell_config2 = new_aspell_config();
    aspell_config_replace(spell_config1, "lang", m_lang1.toAscii());
    aspell_config_replace(spell_config2, "lang", m_lang2.toAscii());
    aspell_config_replace(spell_config1, "encoding", "utf-8");
    aspell_config_replace(spell_config2, "encoding", "utf-8");
}

void SpellChecker::setLanguages(const QString & lang1, const QString & lang2)
{
    m_lang1 = lang1;
    m_lang2 = lang2;
    aspell_config_replace(spell_config1, "lang", m_lang1.toAscii());
    aspell_config_replace(spell_config2, "lang", m_lang2.toAscii());
}

void SpellChecker::spellCheck()
{
     AspellCanHaveError * possible_err = new_aspell_speller(spell_config1);
     spell_checker1 = 0;
     if (aspell_error_number(possible_err) != 0)
       return;
     else
       spell_checker1 = to_aspell_speller(possible_err);
     possible_err = new_aspell_speller(spell_config2);
     spell_checker2 = 0;
     if (aspell_error_number(possible_err) == 0)
            spell_checker2 = to_aspell_speller(possible_err);

     QTextCursor cursor(m_textEdit->document());
     while (!cursor.isNull()&&!cursor.atEnd()) {
        _checkWord(&cursor);
        cursor.movePosition(QTextCursor::WordRight,
                                      QTextCursor::KeepAnchor);
        cursor = m_textEdit->document()->find(*m_regExp, cursor);
     }
     if (!cursor.isNull())
        _checkWord(&cursor);
     delete_aspell_speller(spell_checker1);
     delete_aspell_speller(spell_checker2);
}

void SpellChecker::_checkWord(QTextCursor * cursor)
{
        cursor->select(QTextCursor::WordUnderCursor);
        QString selText = cursor->selectedText();
        static const QRegExp nonDigits("\\D");
        if (!selText.contains(nonDigits))
            return;
        //selText.remove("«");
        //selText.remove("»");
        //selText.remove("\"");
        QByteArray ba = cursor->selectedText().toUtf8();
        if ((aspell_speller_check(spell_checker1, ba.data(), ba.size())== 0) &&
            (aspell_speller_check(spell_checker2, ba.data(), ba.size())== 0)) {
            QTextCharFormat fmt = cursor->charFormat();
            fmt.setUnderlineColor(QColor(Qt::red));
            fmt.setUnderlineStyle(QTextCharFormat::SpellCheckUnderline);
            cursor->setCharFormat(fmt);
        } else {
            QTextCharFormat fmt = cursor->charFormat();
            fmt.setUnderlineStyle(QTextCharFormat::NoUnderline);
            cursor->setCharFormat(fmt);
        }

}

void SpellChecker::checkWord()
{
             AspellCanHaveError * possible_err = new_aspell_speller(spell_config1);
     spell_checker1 = 0;
     if (aspell_error_number(possible_err) != 0)
       return;
     else
       spell_checker1 = to_aspell_speller(possible_err);
     possible_err = new_aspell_speller(spell_config2);
     spell_checker2 = 0;
     if (aspell_error_number(possible_err) == 0)
            spell_checker2 = to_aspell_speller(possible_err);
     QTextCursor cursor = m_textEdit->textCursor();
     _checkWord(&cursor);
     delete_aspell_speller(spell_checker1);
     delete_aspell_speller(spell_checker2);
 }

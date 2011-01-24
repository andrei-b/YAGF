/*
    YAGF - cuneiform OCR graphical front-end
    Copyright (C) 2009 Andrei Borovsky <anb@symmetrica.net>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/

#include <QTextEdit>
#include <QRegExp>
#include <QTextCursor>
#include <QTextCharFormat>
#include <QMap>
#include <QMessageBox>
#include "spellchecker.h"


SpellChecker::SpellChecker(QTextEdit * textEdit):m_textEdit(textEdit)
{
    m_regExp = new QRegExp("[^\\s]*");
    //m_cursor= new QTextCursor(m_textEdit->document());
    spell_config1 = new_aspell_config();
    spell_config2 = new_aspell_config();
    aspell_config_replace(spell_config1, "lang", m_lang1.toAscii());
    aspell_config_replace(spell_config2, "lang", m_lang2.toAscii());
    aspell_config_replace(spell_config1, "encoding", "utf-8");
    aspell_config_replace(spell_config2, "encoding", "utf-8");
    m_map = new StringMap();
    m_map->insert("ruseng", "ru");
    m_map->insert("rus", "ru");
    m_map->insert("bul", "bg");
    m_map->insert("cze", "cs");
    m_map->insert("dan", "da");
    m_map->insert("dut", "nl");
    m_map->insert("eng", "en");
    m_map->insert("est", "et");
    m_map->insert("fra", "fr");
    m_map->insert("ger", "de");
    m_map->insert("hrv", "hr");
    m_map->insert("hun", "hu");
    m_map->insert("ita", "it");
    m_map->insert("lav", "lv");
    m_map->insert("lit", "lt");
    m_map->insert("pol", "pl");
    m_map->insert("por", "pt_PT");
    m_map->insert("rum", "ro");
    m_map->insert("slo", "sl");
    m_map->insert("spa", "es");
    m_map->insert("srp", "sr");
    m_map->insert("swe", "sv");
    m_map->insert("ukr", "uk");
    spell_checker1 = 0;
    spell_checker2 = 0;
    setLanguage("ruseng");
}

void SpellChecker::setLanguage(const QString & lang)
{

     delete_aspell_speller(spell_checker1);
     delete_aspell_speller(spell_checker2);

     m_lang2 = "en";
     m_lang1 = m_map->value(lang, QString("en"));
     if (lang == "rus_fra") {
          m_lang1 = "ru";
          m_lang2 = "fr";
     } else if (lang == "rus_ger") {
          m_lang1 = "ru";
          m_lang2 = "de";
     } else if (lang == "rus_spa") {
          m_lang1 = "ru";
          m_lang2 = "es";
     }
     aspell_config_replace(spell_config1, "lang", m_lang1.toAscii());
     aspell_config_replace(spell_config2, "lang", m_lang2.toAscii());
     AspellCanHaveError * possible_err = new_aspell_speller(spell_config1);
     spell_checker1 = 0;
     if (aspell_error_number(possible_err) == 0)
       spell_checker1 = to_aspell_speller(possible_err);
     possible_err = new_aspell_speller(spell_config2);
     spell_checker2 = 0;
     if (aspell_error_number(possible_err) == 0)
            spell_checker2 = to_aspell_speller(possible_err);

}

void SpellChecker::spellCheck()
{
     if ((spell_checker1 == 0) || (spell_checker2 == 0)) {
         QPixmap icon;
         icon.load(":/warning.png");
         QMessageBox messageBox(QMessageBox::NoIcon, "YAGF", QObject::trUtf8("Required spelling dictionary is not found. Spell-checking is disabled."),
                        QMessageBox::Ok, 0);
                messageBox.setIconPixmap(icon);
                messageBox.exec();
         return;
     }
     QTextCursor cursor(m_textEdit->document());
     while (!cursor.isNull()&&!cursor.atEnd()) {
        _checkWord(&cursor);
        QTextCursor oldc = cursor;
        if (!cursor.movePosition(QTextCursor::NextWord,
                                      QTextCursor::MoveAnchor))
            break;
//cursor.movePosition(QTextCursor::EndOfWord,  QTextCursor::MoveAnchor);

        //cursor = m_textEdit->document()->find(*m_regExp, cursor);
        int oldpos = oldc.position();
        int newpos = cursor.position();
        if (abs(newpos -oldpos) < 3)
            cursor.setPosition(newpos + 1);
     }
     if (!cursor.isNull())
        _checkWord(&cursor);
}

void SpellChecker::unSpellCheck()
{
    QTextCursor cursor(m_textEdit->document());
    QTextCharFormat fmt = cursor.charFormat();
    fmt.setUnderlineStyle(QTextCharFormat::NoUnderline);
    cursor.select(QTextCursor::Document);
    cursor.setCharFormat(fmt);
    cursor.clearSelection();
}

void SpellChecker::_checkWord(QTextCursor * cursor)
{
        cursor->select(QTextCursor::WordUnderCursor);
        QString selText = cursor->selectedText();
        static const QRegExp nonDigits("\\D");
        if (!selText.contains(nonDigits))
            return;
        selText = selText.remove(QString::fromUtf8("«"));
        selText = selText.remove(QString::fromUtf8("»"));
        //selText = selText.remove("\"");
        //selText = selText.remove("(");
        //selText = selText.remove(")");
        QByteArray ba = selText.toUtf8();
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
        cursor->clearSelection();
}

void SpellChecker::checkWord()
{
     if ((spell_checker1 == 0) || (spell_checker2 == 0))
         return;
     QTextCursor cursor = m_textEdit->textCursor();
     _checkWord(&cursor);
 }

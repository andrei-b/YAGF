#ifndef SPELLCHECKER_H
#define SPELLCHECKER_H

#include <QString>
#include <aspell.h>

class QTextEdit;
class QRegExp;
class QTextCursor;

class SpellChecker
{
    public:
    SpellChecker(QTextEdit * textEdit);
    void setLanguages(const QString & lang1, const QString & lang2);
    void spellCheck();
    void checkWord();
    private:
    void _checkWord(QTextCursor * cursor);
    QTextEdit * m_textEdit;
    QRegExp * m_regExp;
    QString m_lang1;
    QString m_lang2;
    AspellConfig * spell_config1;
    AspellConfig * spell_config2;
    AspellSpeller * spell_checker1;
    AspellSpeller * spell_checker2;

};

#endif // SPELLCHECKER_H

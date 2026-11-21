#pragma once
#include <QList>
#include <QString>
#include <optional>

class QWidget;
namespace yagf::ui {
struct OcrLanguageChoice { QString name, code; };
QList<OcrLanguageChoice> installedOcrLanguageChoices();

// Returns the existing ordered language selection plus the chosen, installed code.
std::optional<QString> chooseAdditionalOcrLanguage(QWidget *parent,const QString &selection);
}

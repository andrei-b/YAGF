#pragma once

#include <QString>
#include <optional>

class QWidget;

namespace yagf::ui {

std::optional<QString> editFormulaSource(QWidget *parent, const QString &source, bool display);

}

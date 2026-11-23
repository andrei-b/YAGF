#pragma once

#include "ocrservices.h"

class QWidget;

namespace yagf::ui {

// Returns an edited page only after the user accepts the replacement.
std::optional<core::OcrPage> markInlineFormula(QWidget *parent, const core::OcrPage &page,
    int wordIndex, int start, int length, const QImage &image, QRect crop,
    const processing::OcrService &service, const QString &path, bool unicode);
}

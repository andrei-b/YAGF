#pragma once

#include "ocrdocument.h"
#include <QSet>
#include <QImage>
#include <atomic>

class QPainter;

namespace yagf::processing::detail {

struct PdfPageLayout;
QSet<QString> formulaWordIds(const core::OcrPage &page);
void drawWord(QPainter &painter, const core::OcrWord &word, int dpi, bool formula);
bool drawFormattedPage(QPainter &painter, const PdfPageLayout &layout, const QImage &image,
                       const std::atomic_bool &cancelled, const QSet<QString> &formulaWords = {});

} // namespace yagf::processing::detail

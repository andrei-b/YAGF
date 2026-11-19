#pragma once

#include "ocrdocument.h"
#include <QImage>
#include <atomic>

class QPainter;

namespace yagf::processing::detail {

struct PdfPageLayout;
void drawWord(QPainter &painter, const core::OcrWord &word, int dpi = 150);
bool drawFormattedPage(QPainter &painter, const PdfPageLayout &layout, const QImage &image,
                       const std::atomic_bool &cancelled);

} // namespace yagf::processing::detail

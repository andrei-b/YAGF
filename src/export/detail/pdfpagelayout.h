#pragma once

#include "ocrdocument.h"
#include <QImage>
#include <QRegion>
#include <QSet>
#include <atomic>
#include <functional>
#include <optional>

namespace yagf::processing::detail {

// Export-only page data. Erasure areas and raster retention never enter OcrDocument.
struct PdfPageLayout {
    core::OcrPage page;
    QRegion graphics;
    QSet<QString> rasterWords;
    QVector<QRect> textAreas;
};

// Geometry can come from any local provider; absence/failure preserves saved blocks.
using LayoutRecognizer = std::function<std::optional<core::OcrPage>(
    const QImage &, const QString &language, int pageNumber)>;

PdfPageLayout preparePdfPage(const core::OcrPage &page, const QImage &image,
                             const std::atomic_bool &cancelled,
                             const LayoutRecognizer &recognize = {});

} // namespace yagf::processing::detail

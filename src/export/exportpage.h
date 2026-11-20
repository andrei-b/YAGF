#pragma once

#include "exporttypes.h"
#include "ocrdocument.h"

#include <QImage>
#include <QTemporaryDir>
#include <expected>

namespace yagf::processing {

// Picture blocks have no OCR words, so rebuilding the text hierarchy cannot
// represent them. Include them when exporting the logical document.
QVector<core::OcrRegion> exportRegions(const core::OcrPage &page);

// Recreate the editor's oriented, preprocessed page at OCR resolution.
std::expected<QImage, ExportResult> readExportPage(const core::OcrDocument &document, int number,
                                                   const QTemporaryDir &temporary,
                                                   const std::atomic_bool &cancelled, bool preserveImageDpi = false);

ExportResult writeScanPage(const core::OcrDocument &document, int number, const QString &destination,
                           const QByteArray &format, const std::atomic_bool &cancelled);

} // namespace yagf::processing

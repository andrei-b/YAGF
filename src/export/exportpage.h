#pragma once

#include "exporttypes.h"
#include "ocrdocument.h"

#include <QImage>
#include <QTemporaryDir>
#include <expected>

namespace yagf::processing {

// Recreate the editor's oriented, preprocessed page at OCR resolution.
std::expected<QImage, ExportResult> readExportPage(const core::OcrDocument &document, int number,
                                                   const QTemporaryDir &temporary,
                                                   const std::atomic_bool &cancelled);

} // namespace yagf::processing

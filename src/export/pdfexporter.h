#pragma once

#include "exporttypes.h"
#include "ocrdocument.h"

#include <atomic>

namespace yagf::processing {

using PdfExportResult = ExportResult;

class PdfExporter {
public:
    enum class Mode : quint8 { SearchableScan, FormattedText };
    using Progress = ExportProgress;

    [[nodiscard]] static PdfExportResult write(const core::OcrDocument &document,
                                               const QString &destination, bool archive,
                                               const std::atomic_bool &cancelled,
                                               const Progress &progress = {},
                                               Mode mode = Mode::SearchableScan);
};

} // namespace yagf::processing

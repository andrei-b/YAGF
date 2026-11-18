#pragma once

#include "ocrdocument.h"

#include <QString>

#include <atomic>
#include <functional>

namespace yagf::processing {

struct ExportResult {
    enum class Status { Success, Cancelled, Error } status = Status::Error;
    QString error;
};

using PdfExportResult = ExportResult;

class PdfExporter {
public:
    using Progress = std::function<void(int completed, int total)>;

    [[nodiscard]] static PdfExportResult write(const core::OcrDocument &document,
                                               const QString &destination, bool archive,
                                               const std::atomic_bool &cancelled,
                                               const Progress &progress = {});
};

class ScientificBookExporter {
public:
    [[nodiscard]] static ExportResult write(const core::OcrDocument &document, const QString &destination,
                                            const std::atomic_bool &cancelled,
                                            const PdfExporter::Progress &progress = {});
};

class VerifiedSampleExporter {
public:
    [[nodiscard]] static ExportResult write(const core::OcrDocument &document, const QString &destination,
                                            const std::atomic_bool &cancelled,
                                            const PdfExporter::Progress &progress = {});
};

} // namespace yagf::processing

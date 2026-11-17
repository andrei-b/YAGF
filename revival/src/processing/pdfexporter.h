#pragma once

#include "ocrdocument.h"

#include <QString>

#include <atomic>
#include <functional>

namespace yagf::processing {

struct PdfExportResult {
    enum class Status { Success, Cancelled, Error } status = Status::Error;
    QString error;
};

class PdfExporter {
public:
    using Progress = std::function<void(int completed, int total)>;

    [[nodiscard]] static PdfExportResult write(const core::OcrDocument &document,
                                               const QString &destination, bool archive,
                                               const std::atomic_bool &cancelled,
                                               const Progress &progress = {});
};

} // namespace yagf::processing

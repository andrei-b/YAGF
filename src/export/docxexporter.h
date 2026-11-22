#pragma once

#include "exporttypes.h"
#include "ocrdocument.h"
#include <atomic>

namespace yagf::processing {

class DocxExporter {
public:
    static ExportResult write(const core::OcrDocument &document, const QString &destination,
                              const std::atomic_bool &cancelled, const ExportProgress &progress = {});
};

} // namespace yagf::processing

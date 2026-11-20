#pragma once
#include "ocrdocument.h"
#include "exporttypes.h"

namespace yagf::processing {
class LatexDocumentExporter {
public:
    static ExportResult write(const core::OcrDocument &document,const QString &destination,
                               const std::atomic_bool &cancelled,const ExportProgress &progress = {});
};
} // namespace yagf::processing

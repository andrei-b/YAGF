#pragma once

#include <QString>

namespace yagf::core {

class OcrDocument;

struct DocumentContext {
    static constexpr int maximumDescriptionLength = 4000;
    QString description;
    bool inferFromRecognizedText = true;
    bool operator==(const DocumentContext &) const = default;
};

struct ReconstructionContext {
    QString description;
    QString recognizedText;
};

// Rebuild from current evidence for each review, so corrections and Undo take effect.
ReconstructionContext reconstructionContext(const OcrDocument &document, int pageNumber, int firstWord);

} // namespace yagf::core

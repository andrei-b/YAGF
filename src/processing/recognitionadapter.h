#pragma once
#include "ocrdocument.h"

namespace yagf::processing {
// Geometry-free output becomes a single page-level editing block whose extent
// is explicitly inferred. It never receives fabricated word boxes/source boxes.
std::expected<core::OcrPage, QString> recognitionToPage(const core::RecognitionResult &result);
// Bridge for word-oriented parsers and existing page fixtures. New VLM engines
// should construct native regions directly instead of passing through OcrPage.
core::RecognitionResult recognitionFromPage(const core::OcrPage &page);
void appendRecognitionEvidence(core::OcrPage &page, const core::OcrPage &reading, QPoint offset = {});
}

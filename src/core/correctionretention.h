#pragma once
#include <expected>

#include "ocrdocument.h"

namespace yagf::core {

struct RetentionResult { int retained = 0; int pending = 0; };

// Only unambiguous text/geometry matches are applied. Unmatched verified work
// stays in pendingCorrections, and is never silently treated as a new OCR reading.
RetentionResult retainCorrections(OcrPage &fresh, const OcrPage &previous, bool preserveFormatting = false);
OcrPage rescalePageGeometry(const OcrPage &page, int width, int height,
                            int sourceWidth, int sourceHeight, int dpi);

// Apply only after the user checks the saved reading against the current scan.
std::expected<OcrPage, QString> restorePendingCorrection(const OcrPage &page, const QString &id,
                                                        const QString &reading, const QVector<int> &selection);
}

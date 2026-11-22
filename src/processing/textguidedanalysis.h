#pragma once

#include "iocrengine.h"
#include <optional>

namespace yagf::processing {

struct TextGuidance {
    std::optional<double> skewDegrees;
    int inkThreshold = 0;
    int wordCount = 0;
    int lineCount = 0;
};

// Estimate the residual tilt in the current processed frame from independent
// lines of measured word boxes. Synthetic crop extents cannot supply a slope.
TextGuidance recognizedTextGuidance(const QVector<core::OcrWord> &words);
double recognitionGuidedSkew(double rasterAngle, const TextGuidance &text);

struct LayoutPage {
    QImage image;
    core::OcrPage page;
};
std::expected<LayoutPage, QString> prepareLayoutPage(const QImage &raw, const QImage &displayed,
    core::OcrPage page, int pdfDpi, const QString &language, std::stop_token stop = {});

// The pilot is upright (quarter turns already applied), before fine deskew or binarization.
std::expected<TextGuidance, OcrError> analyzeTextPilot(const QImage &pilot, const QString &language,
                                                     std::stop_token stop = {});
}

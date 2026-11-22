#include "textguidedanalysis.h"
#include "imageprocessor.h"
#include "pagegeometry.h"
#include "tesseractengine.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>
#include <tuple>

namespace yagf::processing {
namespace {

double median(QVector<double> values)
{
    std::ranges::sort(values);
    return (values[(values.size() - 1) / 2] + values[values.size() / 2]) / 2;
}

// A median of widely separated word-pair slopes tolerates descenders and bad boxes.
// Each line contributes one vote, including lines from different columns.
std::optional<double> lineAngle(QVector<QRect> words)
{
    if (words.size() < 3) return {};
    std::ranges::sort(words, {}, &QRect::left);
    QVector<double> angles;
    const int stride = (words.size() + 31) / 32;
    for (int i = 0; i < words.size(); i += stride) {
        const auto &a = words[i];
        for (int j = i + stride; j < words.size(); j += stride) {
            const auto &b = words[j];
            const int dx = b.center().x() - a.center().x();
            if (dx < 5 * std::max(a.height(), b.height())) continue;
            angles.append(std::atan2(b.bottom() - a.bottom(), dx) * 180 / std::numbers::pi);
        }
    }
    if (angles.isEmpty()) return {};
    return median(angles);
}

bool reliable(const core::OcrWord &word)
{
    return !word.layoutBlock && !word.geometryInferred && !word.bounds.isEmpty() &&
           word.confidence >= 65 && word.text.size() >= 2 && !word.text.contains('\n');
}

} // namespace

TextGuidance recognizedTextGuidance(const QVector<core::OcrWord> &words)
{
    std::map<std::tuple<int,int,int>, QVector<QRect>> lines;
    TextGuidance result;
    for (const auto &word : words) {
        if (!reliable(word)) continue;
        ++result.wordCount;
        lines[{word.block, word.paragraph, word.line}].append(word.bounds);
    }

    QVector<double> angles;
    for (const auto &[id, words] : lines) {
        if (const auto angle = lineAngle(words)) angles.append(*angle);
    }
    if (angles.size() >= 2) {
        const double center = median(angles);
        QVector<double> inliers;
        for (double angle : angles)
            if (std::abs(angle - center) <= 1.0) inliers.append(angle);
        if (inliers.size() >= 2 && 3 * inliers.size() >= 2 * angles.size()) {
            const double skew = median(inliers);
            if (std::abs(skew) <= 15) {
                result.skewDegrees = skew;
                result.lineCount = inliers.size();
            }
        }
    }
    return result;
}

double recognitionGuidedSkew(double rasterAngle, const TextGuidance &text)
{
    if (!text.skewDegrees) return rasterAngle;
    const double angle = *text.skewDegrees;
    if (std::abs(angle - rasterAngle) <= 1.)
        return rasterAngle == 0 ? angle : (angle + rasterAngle) / 2;
    // Three agreeing text lines can distinguish body text from sloping
    // diagram edges and rules that dominate a pixel projection.
    return text.lineCount >= 3 ? angle : rasterAngle;
}

std::expected<LayoutPage, QString> prepareLayoutPage(const QImage &raw, const QImage &displayed,
    core::OcrPage page, int pdfDpi, const QString &language, std::stop_token stop)
{
    if (displayed.size() != QSize(page.width, page.height) ||
        raw.size() != QSize(page.sourceWidth, page.sourceHeight))
        return std::unexpected(QStringLiteral("Layout image dimensions differ from the saved page."));
    if (!page.coordinates.isDefined()) {
        auto origin = ImageProcessor::coordinatePipeline(raw.size(), page.preprocessing, pdfDpi);
        if (!origin) return std::unexpected(origin.error());
        page.coordinates = *origin;
    }
    auto assessment = ImageProcessor::assess(displayed);
    if (!assessment) return std::unexpected(assessment.error());
    auto text = recognizedTextGuidance(page.words);
    if (!text.skewDegrees && std::abs(assessment->skewDegrees) >= .35 && !stop.stop_requested()) {
        // On a first layout pass there may be no saved words. Ask local OCR
        // for line geometry on a bounded pilot before rotating a whole page.
        const auto pilot = displayed.width() > 1400 || displayed.height() > 1400
            ? displayed.scaled(1400, 1400, Qt::KeepAspectRatio, Qt::SmoothTransformation) : displayed;
        auto reading = TesseractEngine().recognize(pilot, {.language = TesseractEngine::modelLanguage(language)}, stop);
        if (reading) text = recognizedTextGuidance(reading->words);
    }
    if (stop.stop_requested()) return std::unexpected(QStringLiteral("Cancelled"));
    const double residual = recognitionGuidedSkew(assessment->skewDegrees, text);
    auto settings = page.preprocessing;
    settings.deskewDegrees = std::round((settings.deskewDegrees + residual) * 10) / 10;
    if (!text.skewDegrees || std::abs(*text.skewDegrees) < .35 || std::abs(residual) < .35 ||
        (text.lineCount < 3 && std::abs(*text.skewDegrees - assessment->skewDegrees) > 1.) ||
        std::abs(settings.deskewDegrees) > 15)
        return LayoutPage{displayed, std::move(page)};
    auto coordinates = ImageProcessor::coordinatePipeline(raw.size(), settings, pdfDpi);
    if (!coordinates) return std::unexpected(coordinates.error());
    auto mapped = core::remapPageGeometry(page, *coordinates);
    if (!mapped) return std::unexpected(mapped.error());
    auto image = ImageProcessor::apply(raw, settings);
    if (!image) return std::unexpected(image.error());
    mapped->preprocessing = std::move(settings);
    return LayoutPage{std::move(*image), std::move(*mapped)};
}

std::expected<TextGuidance, OcrError> analyzeTextPilot(const QImage &pilot, const QString &language,
                                                     std::stop_token stop)
{
    auto page = TesseractEngine().recognize(pilot, {.language = language}, stop);
    if (!page) return std::unexpected(page.error());
    double pilotSkew = 0;
    QTransform toSource;
    if (std::ranges::count_if(page->words, reliable) < 3 && !stop.stop_requested()) {
        const auto normalized = ImageProcessor::normalizedTextPilot(pilot);
        const auto estimate = ImageProcessor::assess(normalized);
        if (!estimate) return std::unexpected(OcrError{estimate.error()});
        const auto aligned = ImageProcessor::apply(normalized, {.deskewDegrees = estimate->skewDegrees});
        if (!aligned) return std::unexpected(OcrError{aligned.error()});
        auto retry = TesseractEngine().recognize(*aligned, {.language = language}, stop);
        if (!retry) return std::unexpected(retry.error());
        if (std::ranges::count_if(retry->words, reliable) > std::ranges::count_if(page->words, reliable)) {
            page = std::move(retry);
            pilotSkew = estimate->skewDegrees;
            toSource = QImage::trueMatrix(QTransform().rotate(-pilotSkew), pilot.width(), pilot.height()).inverted();
        }
    }
    auto result = recognizedTextGuidance(page->words);
    if (result.skewDegrees) {
        *result.skewDegrees += pilotSkew;
        if (std::abs(*result.skewDegrees) > 15) result.skewDegrees.reset();
    }
    QVector<QRect> boxes;
    for (const auto &word : page->words)
        if (reliable(word)) boxes.append(toSource.mapRect(word.bounds));
    if (stop.stop_requested()) return std::unexpected(OcrError{{}, true});
    result.inkThreshold = ImageProcessor::estimateInkThreshold(pilot, boxes);
    return result;
}
}

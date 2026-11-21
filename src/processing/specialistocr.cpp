#include "specialistocr.h"

#include <QRegularExpression>
#include <algorithm>

namespace yagf::processing {
namespace {
bool looksLikeFormula(const QString &line)
{
    static const QRegularExpression math(QStringLiteral("[∑∏∫√≈≠≤≥∞∂∇]|[\\p{L}\\p{N})]\\s*=\\s*[+−(\\p{L}\\p{N}-]"));
    return line.contains(math);
}
}

QStringList collectSpecialistReadings(core::OcrPage &page, const QImage &image,
    const QVector<QRect> &formulaRegions, IOcrEngine *math, IOcrEngine *prose, const std::stop_token &stop)
{
    QStringList errors;
    for (int begin = 0; begin < page.words.size() && (math || prose) && !stop.stop_requested();) {
        const auto &first = page.words.at(begin);
        QStringList tokens;
        QRect bounds;
        bool weak = false, reviewed = false;
        int end = begin;
        while (end < page.words.size()) {
            const auto &word = page.words.at(end);
            if (word.block != first.block || word.paragraph != first.paragraph || word.line != first.line) break;
            tokens.append(word.text);
            bounds |= word.bounds;
            weak |= word.confidence >= 0 && word.confidence < 70;
            reviewed |= word.reviewed;
            ++end;
        }
        const int anchor = begin;
        begin = end;
        if (reviewed || tokens.size() > 1024 || !image.rect().contains(bounds)) continue;
        const QString baseline = tokens.join(' ');
        const bool formula = looksLikeFormula(baseline) ||
            std::ranges::any_of(formulaRegions,
                                [&](const QRect &region) { return region.contains(bounds.center()); });
        IOcrEngine *engine = formula && math ? math : weak ? prose : nullptr;
        if (!engine) continue;
        // A small margin retains antialiased edges without submitting adjacent passages.
        const QRect crop = bounds.adjusted(-2, -2, 2, 2).intersected(image.rect());
        auto result = engine->recognize(image.copy(crop), {.language = "auto", .pageNumber = page.number}, stop);
        if (!result) {
            if (result.error().cancelled || stop.stop_requested()) break;
            errors.append(engine->id() + ": " + result.error().message);
            if (engine == math) math = nullptr;
            if (engine == prose) prose = nullptr;
            continue;
        }
        appendRecognitionEvidence(page, *result, crop.topLeft());
        core::OcrDocument excerpt;
        excerpt.pages.append(std::move(*result));
        if (const QString reading = excerpt.plainText().trimmed(); !reading.isEmpty() && reading.size() <= 16384 && reading != baseline)
            page.words[anchor].alternative = {engine->id(), reading, tokens, formula && engine == math};
    }
    return errors;
}

} // namespace yagf::processing

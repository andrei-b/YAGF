#include "spellingformulas.h"
#include "mathocrengine.h"
#include "spellchecker.h"
#include "wordspelling.h"

#include <QRegularExpression>
#include <algorithm>

namespace yagf::processing {
namespace {
bool mathematicalReading(const QString &text)
{
    // A model can transcribe prose as \\text{...}; that is not a formula.
    static const QRegularExpression prose(QStringLiteral("\\\\(?:text|textrm|textsf|textbf|textit|mbox)\\s*\\{[^{}]*\\}"));
    static const QRegularExpression structure(QStringLiteral(
        "[=+*/^_<>×÷±∓∑∏∫√≤≥≠≈≡∞∂∇²³₀-₉−]|"
        "\\\\(?:frac|dfrac|tfrac|sqrt|sum|prod|int|lim|sin|cos|tan|log|ln|exp|alpha|beta|gamma|delta|theta|lambda|mu|pi|sigma|omega)\\b"));
    QString math = text;
    math.remove(prose);
    return math.contains(structure);
}

QString languageAt(const core::OcrPage &page, const core::OcrWord &word)
{
    for (const auto &region : page.imageRegions)
        if (!region.language.isEmpty() && region.bounds.contains(word.bounds.center())) return region.language;
    return page.language;
}
}

std::expected<QStringList, OcrError> collectSpellingFormulaReadings(
    core::OcrPage &page, const QImage &image, std::stop_token stop)
{
    if (stop.stop_requested()) return std::unexpected(OcrError{{}, true});
    if (MathOcrEngine::pythonPath().isEmpty()) return QStringList{};
    core::SpellChecker checker;
    const auto spelling = core::wordSpelling(page);
    QSet<int> existingReadings;
    for (int i = 0; i < page.words.size(); ++i)
        for (int covered : page.specialistSpan(i)) existingReadings.insert(covered);
    struct Candidate { QVector<int> indices; QRect bounds; QString language; };
    QVector<Candidate> candidates;
    for (int i = 0; i < page.words.size(); ++i) {
        if (stop.stop_requested()) return std::unexpected(OcrError{{}, true});
        const auto &word = page.words[i];
        if (word.reviewed || word.layoutBlock || word.text.size() > 1024 || existingReadings.contains(i) ||
            spelling.continuations.contains(i) || word.bounds.isEmpty() || !image.rect().contains(word.bounds)) continue;
        if (std::ranges::any_of(page.imageRegions, [&](const auto &r) {
            return (r.notText || (!r.autoDetected && r.content == "text")) && r.bounds.contains(word.bounds.center());
        })) continue;
        const QString language = languageAt(page, word);
        // "auto" has no known source language. Do not guess from the UI locale.
        if (language.isEmpty() || language == "auto") continue;
        checker.setLanguages({language});
        if (!std::ranges::any_of(spelling.tokens[i], [&](const auto &token) { return checker.isMisspelled(token); })) continue;
        if (!candidates.isEmpty()) {
            auto &last = candidates.last();
            const auto &previous = page.words[last.indices.last()];
            if (last.indices.size() < 64 && last.indices.last() == i-1 && last.language == language && previous.block == word.block &&
                previous.paragraph == word.paragraph && previous.line == word.line &&
                word.bounds.left() >= previous.bounds.left() &&
                word.bounds.left()-previous.bounds.right() <= 2*std::max(word.bounds.height(), previous.bounds.height())) {
                last.indices.append(i);
                last.bounds |= word.bounds;
                continue;
            }
        }
        candidates.append({{i}, word.bounds, language});
    }
    if (candidates.isEmpty()) return QStringList{};
    constexpr int limit = 128;
    QStringList warnings;
    if (candidates.size() > limit) {
        candidates.resize(limit);
        warnings.append(QStringLiteral("Formula retries were limited to 128 suspicious word spans; remaining words are available in the verification queue."));
    }
    QList<QRect> boxes;
    for (const auto &candidate : candidates) boxes.append(candidate.bounds.adjusted(-2, -2, 2, 2).intersected(image.rect()));
    MathOcrEngine math;
    auto result = math.recognizeBoxes(image, {.language = "auto", .pageNumber = page.number}, stop, boxes, &warnings);
    if (!result) {
        if (result.error().cancelled || stop.stop_requested()) return std::unexpected(result.error());
        warnings.append(result.error().message + QStringLiteral(" Kept native OCR for suspicious words."));
        return warnings;
    }
    for (const auto &reading : result->words) {
        if (!mathematicalReading(reading.text)) continue;
        const int index = boxes.indexOf(reading.bounds);
        if (index < 0) continue;
        const auto &candidate = candidates[index];
        QStringList tokens;
        for (int i : candidate.indices) tokens.append(page.words[i].text);
        if (reading.text.simplified() == tokens.join(' ').simplified()) continue;
        page.words[candidate.indices.first()].alternative = {math.id(), reading.text, tokens, true};
    }
    return warnings;
}
}

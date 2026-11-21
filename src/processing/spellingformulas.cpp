#include "spellingformulas.h"
#include "compactassignment.h"
#include "mathocrengine.h"
#include "tesseractengine.h"
#include "spellchecker.h"
#include "wordspelling.h"

#include <QRegularExpression>
#include <QPainter>
#include <QElapsedTimer>
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

QString languageAt(const core::OcrPage &page, const core::OcrWord &word, const QString &fallback = {})
{
    for (const auto &region : page.imageRegions)
        if (!region.language.isEmpty() && region.bounds.contains(word.bounds.center())) return region.language;
    return page.language.isEmpty() || page.language == "auto" ? fallback : page.language;
}

bool formulaWord(const core::OcrPage &page, const core::OcrWord &word)
{
    if (!word.layoutBlock) return false;
    return word.readingEngine == "rapid-latex-ocr" || page.engineId == "rapid-latex-ocr" ||
        std::ranges::any_of(page.regions, [&](const auto &region) {
            return (region.type == "equation" || region.type == "formula") && region.bounds.contains(word.bounds.center());
        });
}

bool dictionaryText(const core::OcrPage &reading, core::SpellChecker &checker)
{
    // Digits, operators and LaTeX are not evidence for changing math to prose.
    static const QRegularExpression prose(QStringLiteral("^[\\p{L}\\p{M}\\s'’\"“”„«».,;:!?()\\[\\]‐‑–—\u00ad-]+$"));
    const auto spelling = core::wordSpelling(reading);
    bool lexicalWord = false;
    for (int i = 0; i < reading.words.size(); ++i) {
        const auto &word = reading.words[i];
        if (!prose.match(word.text).hasMatch() || word.confidence < 70) return false;
        for (const auto &token : spelling.tokens[i]) {
            if (checker.isMisspelled(token)) return false;
            lexicalWord |= token.size() >= 2;
        }
    }
    return lexicalWord;
}
}

std::expected<QStringList, OcrError> collectSpellingFormulaReadings(
    core::OcrPage &page, const QImage &image, std::stop_token stop, OcrProgress progress, IOcrEngine *formulaProvider)
{
    if (stop.stop_requested()) return std::unexpected(OcrError{{}, true});
    if (!formulaProvider && MathOcrEngine::pythonPath().isEmpty()) return QStringList{};
    core::SpellChecker checker;
    const auto spelling = core::wordSpelling(page);
    const auto assignments = compactAssignmentWords(page.words);
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
        const bool assignment = assignments.contains(i);
        if (!assignment && (language.isEmpty() || language == "auto")) continue;
        checker.setLanguages({language});
        if (!assignment && !std::ranges::any_of(spelling.tokens[i], [&](const auto &token) { return checker.isMisspelled(token); })) continue;
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
    if (progress) progress(QStringLiteral("Checking suspicious text for formulas"),0,boxes.size());
    auto result = recognizeFormulaBoxes(formulaProvider, image, {.language = "auto", .pageNumber = page.number,.progress=progress}, stop, boxes, &warnings,30000);
    if (!result) {
        if (result.error().cancelled || stop.stop_requested()) return std::unexpected(result.error());
        warnings.append(result.error().message + QStringLiteral(" Kept native OCR for suspicious words."));
        return warnings;
    }
    appendRecognitionEvidence(page, *result);
    for (const auto &reading : result->words) {
        if (!mathematicalReading(reading.text)) continue;
        const int index = boxes.indexOf(reading.bounds);
        if (index < 0) continue;
        const auto &candidate = candidates[index];
        QStringList tokens;
        for (int i : candidate.indices) tokens.append(page.words[i].text);
        if (reading.text.simplified() == tokens.join(' ').simplified()) continue;
        page.words[candidate.indices.first()].alternative = {reading.readingEngine, reading.text, tokens, true};
    }
    return warnings;
}

std::expected<QStringList, OcrError> collectFormulaTextReadings(
    core::OcrPage &page, const QImage &image, const QString &language, std::stop_token stop, OcrProgress progress)
{
    if (stop.stop_requested()) return std::unexpected(OcrError{{}, true});
    core::SpellChecker checker;
    TesseractEngine text;
    QStringList warnings;
    QSet<QString> failedLanguages;
    int attempts = 0;
    QElapsedTimer budget; budget.start();
    for (auto &word : page.words) {
        if (stop.stop_requested()) return std::unexpected(OcrError{{}, true});
        if (!formulaWord(page, word) || word.reviewed || !word.alternative.text.isEmpty() ||
            word.bounds.isEmpty() || !image.rect().contains(word.bounds)) continue;
        if (std::ranges::any_of(page.imageRegions, [&](const auto &region) {
            return (region.notText || (!region.autoDetected && region.content == "math")) &&
                region.bounds.contains(word.bounds.center());
        })) continue;
        const QString selected = languageAt(page, word, language);
        if (selected.isEmpty() || selected == "auto" || failedLanguages.contains(selected)) continue;
        checker.setLanguages({selected});
        if (checker.activeDictionaries().isEmpty()) continue;
        if (budget.elapsed()>30000) {warnings.append(QStringLiteral("Kept remaining formulas after the text-retry time budget was reached. Select a smaller block to retry.")); break;}
        if (++attempts > 128) {
            warnings.append(QStringLiteral("Text retries were limited to 128 formula crops; remaining formulas are available in the verification queue."));
            break;
        }
        if (progress) progress(QStringLiteral("Checking formulas as text"),attempts,std::min(128,int(page.words.size())));
        // White margins help Tesseract recognize a short isolated word.
        const QRect bounds = word.bounds.adjusted(-2, -2, 2, 2).intersected(image.rect());
        QImage crop(bounds.size()+QSize(16, 16), QImage::Format_RGB32);
        crop.fill(Qt::white);
        { QPainter painter(&crop); painter.drawImage(QPoint(8, 8), image, bounds); }
        auto reading = text.recognize(crop, {.language = selected, .pageNumber = page.number, .singleTextLine = true}, stop);
        if (!reading) {
            if (reading.error().cancelled || stop.stop_requested()) return std::unexpected(reading.error());
            warnings.append(reading.error().message + QStringLiteral(" Kept the formula reading."));
            // A missing model or failed executable affects every crop in this language.
            failedLanguages.insert(selected);
            continue;
        }
        appendRecognitionEvidence(page, *reading, bounds.topLeft()-QPoint(8, 8));
        if (!dictionaryText(*reading, checker)) continue;
        core::OcrDocument excerpt;
        excerpt.pages.append(std::move(*reading));
        const QString alternative = excerpt.plainText().trimmed();
        if (alternative.isEmpty() || alternative.size() > 16384 || alternative == word.text) continue;
        word.alternative = {text.id(), alternative, {word.text}, false};
    }
    return warnings;
}
}

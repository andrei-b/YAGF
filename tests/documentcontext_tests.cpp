#include "documentcontext.h"
#include "ocrdocument.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

using namespace yagf::core;

namespace {
OcrWord word(const QString &text, double confidence = 96, bool reviewed = false)
{
    OcrWord result;
    result.text = result.originalText = text;
    result.confidence = confidence;
    result.reviewed = reviewed;
    return result;
}

OcrPage page(int number, const QString &language, QVector<OcrWord> words)
{
    OcrPage result;
    result.number = number;
    result.language = language;
    result.words = std::move(words);
    return result;
}
}

int main()
{
    OcrDocument document;
    document.language = "eng";
    document.context.description = "A textbook about thermodynamics.";
    // Deliberately unsorted: use source page numbers, not insertion order.
    document.pages = {
        page(4, "eng", {word("Later chapter")}),
        page(2, "deu", {word("Other language")}),
        page(3, "eng", {word("Entropy"), word("garb1ed", 20), word("increases"),
                        word("uncertain-target", 40), word("Following text")}),
        page(1, "eng", {word("Thermodynamics"), word("unverified-edit"), word("reviewed", -1, true)})};
    document.pages[3].words[1].originalText = "different-original";
    const auto context = reconstructionContext(document, 3, 3);
    assert(context.description == document.context.description);
    assert(context.recognizedText.contains("Thermodynamics"));
    assert(context.recognizedText.contains("reviewed"));
    assert(context.recognizedText.contains("Entropy […] increases"));
    for (const auto &excluded : {"garb1ed", "unverified-edit", "uncertain-target", "Following text", "Later chapter", "Other language"})
        assert(!context.recognizedText.contains(excluded));
    assert(context.recognizedText.indexOf("Page 1") < context.recognizedText.indexOf("Page 3"));

    document.pages[2].words[1].text = "always";
    document.pages[2].words[1].reviewed = true;
    assert(reconstructionContext(document, 3, 3).recognizedText.contains("Entropy always increases"));
    document.context.inferFromRecognizedText = false;
    assert(reconstructionContext(document, 3, 3).recognizedText.isEmpty());
    assert(reconstructionContext(document, 3, 3).description == document.context.description);
    document.context.description.clear();
    document.context.inferFromRecognizedText = true;
    assert(!reconstructionContext(document, 3, 3).recognizedText.isEmpty());
    assert(reconstructionContext(document, 3, 3).description.isEmpty());
    assert(reconstructionContext(document, 1, 0).recognizedText.isEmpty());
    assert(reconstructionContext(document, 99, 0).recognizedText.isEmpty());
    assert(reconstructionContext(document, 3, -1).recognizedText.isEmpty());
    assert(reconstructionContext(document, 3, 999).recognizedText.isEmpty());

    // Large books contribute at most three recent eligible page excerpts.
    document.pages.clear();
    for (int number = 1; number <= 20; ++number) {
        QVector<OcrWord> words;
        for (int i = 0; i < 1000; ++i) words.append(word(QStringLiteral("terminology%1").arg(i)));
        document.pages.append(page(number, "eng", std::move(words)));
    }
    const auto bounded = reconstructionContext(document, 20, 1000).recognizedText;
    assert(bounded.size() <= 6000 && bounded.contains("terminology999"));
    assert(bounded.contains("Page 18 ") && bounded.contains("Page 20 "));
    assert(!bounded.contains("Page 17 "));
    // A reviewed structured block can also provide context without word confidence.
    document.pages[19].words = {word(QString(20000, QLatin1Char('a')), -1, true)};
    document.pages[19].words[0].layoutBlock = true;
    assert(reconstructionContext(document, 20, 1).recognizedText.size() <= 6000);
}

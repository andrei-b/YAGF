#include "documentcontext.h"
#include "ocrdocument.h"

#include <algorithm>

namespace yagf::core {
namespace {

QString excerpt(const OcrPage &page, qsizetype end)
{
    constexpr int limit = 1800;
    QString result;
    const OcrWord *next = nullptr;
    bool gap = false;
    for (qsizetype i = end; i > 0; --i) {
        const auto &word = page.words[i - 1];
        if ((!word.reviewed && (word.confidence < 90 || word.text != word.originalText)) ||
            word.text.trimmed().isEmpty()) {
            gap = true;
            continue;
        }
        const QString text = word.text.simplified();
        const QString separator = gap ? QStringLiteral(" […] ") : !next ? QString() :
            (word.block == next->block && word.paragraph == next->paragraph && word.line == next->line)
                ? QStringLiteral(" ") : QStringLiteral("\n");
        if (text.size() + separator.size() + result.size() > limit) {
            if (result.isEmpty()) result = text.left(limit - 4) + QStringLiteral(" […]");
            break;
        }
        result.prepend(text + separator);
        next = &word;
        gap = false;
    }
    if (gap && !result.isEmpty() && result.size() + 4 <= limit) result.prepend(QStringLiteral("[…] "));
    return result.trimmed();
}

} // namespace

ReconstructionContext reconstructionContext(const OcrDocument &document, int pageNumber, int firstWord)
{
    ReconstructionContext result{document.context.description.left(DocumentContext::maximumDescriptionLength), {}};
    if (!document.context.inferFromRecognizedText) return result;
    const auto *current = document.page(pageNumber);
    if (!current || firstWord < 0 || firstWord > current->words.size()) return result;
    const QString language = current->language.isEmpty() ? document.language : current->language;
    QVector<const OcrPage *> pages;
    for (const auto &page : document.pages) {
        const QString pageLanguage = page.language.isEmpty() ? document.language : page.language;
        if (page.number <= pageNumber && pageLanguage == language) pages.append(&page);
    }
    std::ranges::sort(pages, [](const OcrPage *a, const OcrPage *b) { return a->number > b->number; });
    QStringList excerpts;
    for (const auto *page : pages) {
        const QString text = excerpt(*page, page == current ? firstWord : page->words.size());
        if (text.isEmpty()) continue;
        excerpts.prepend(QStringLiteral("Page %1 (earlier text; omissions marked […]):\n%2")
            .arg(page->number).arg(text));
        if (excerpts.size() == 3) break;
    }
    result.recognizedText = excerpts.join(QStringLiteral("\n\n"));
    return result;
}

} // namespace yagf::core

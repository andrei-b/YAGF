#include "correctionmemory.h"
#include "wordeditor.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

using namespace yagf::core;

static OcrPage makePage(int number, const QStringList &tokens)
{
    OcrPage page;
    page.number = number;
    page.width = page.sourceWidth = 20 * static_cast<int>(tokens.size());
    page.height = page.sourceHeight = 20;
    for (const auto &token : tokens) {
        const QRect box(static_cast<int>(page.words.size()) * 20, 0, 18, 15);
        page.words.append({number, 1, 1, 1, box, token, token, 90});
    }
    page.rebuildHierarchy();
    return page;
}

int main()
{
    OcrDocument book;
    book.sourcePath = "book.pdf";
    book.totalPages = 3;
    book.replacePage(makePage(1, {"class", "QWldget", "inherits"}));
    book.replacePage(makePage(2, {"macro", "QWldget", "expands"}));
    book.replacePage(makePage(3, {"class", "QWldget", "inherits"}));
    const OcrPage &target = *book.page(3);
    assert(CorrectionMemory(book).suggestions(target, 1).isEmpty());
    auto first = WordEditor::apply(*book.page(1), {1}, {{"QWidget"}});
    auto second = WordEditor::apply(*book.page(2), {1}, {{"QWIDGET"}});
    assert(first && second);
    book.replacePage(*first);
    book.replacePage(*second);
    OcrDocument reopened;
    QString error;
    assert(OcrDocument::fromJson(book.toJson(), &reopened, &error));
    CorrectionMemory memory(reopened);
    auto matches = memory.suggestions(*reopened.page(3), 1);
    assert(matches.size() == 2 && matches[0].exact && matches[0].replacement == "QWidget");
    assert(matches[0].matchingNeighbors == 2 && matches[1].matchingNeighbors == 0);
    assert(memory.suggestions(*first, 1).isEmpty());
    for (const QString &variant : QStringList{"QWldgat", "QWldgett", "QWldgt"}) {
        matches = memory.suggestions(makePage(1, {variant}), 0);
        assert(matches.size() == 2 && !matches.first().exact);
    }
    for (const QString &token : QStringList{"QWldge7", "QWldge_", "QWd", "QObject"})
        assert(memory.suggestions(makePage(1, {token}), 0).isEmpty());
    assert(CorrectionMemory().suggestions(*reopened.page(3), 1).isEmpty());
    book.replacePage(*first); // Replacing a page must not double-count a correction.
    book.replacePage(makePage(2, {"QWldget"}));
    matches = CorrectionMemory(book).suggestions(*book.page(3), 1);
    assert(matches.size() == 1 && matches[0].occurrences == 1);

    OcrDocument passage;
    auto corrected = WordEditor::apply(makePage(1, {"QWldget", "and", "QWldget"}), {0, 1, 2},
                                       {{"QWidget"}, {"and"}, {"QWldget"}});
    assert(corrected && !corrected->words[2].reviewed);
    passage.replacePage(*corrected);
    assert(CorrectionMemory(passage).suggestions(*corrected, 2).first().replacement == "QWidget");

    OcrDocument identifiers;
    const auto technical = WordEditor::apply(makePage(1, {"I001", "Qt::All"}), {0, 1},
                                            {{"1001"}, {"Qt::ALL"}});
    assert(technical);
    identifiers.replacePage(*technical);
    const CorrectionMemory identifierMemory(identifiers);
    assert(identifierMemory.suggestions(makePage(1, {"I001"}), 0).first().replacement == "1001");
    assert(identifierMemory.suggestions(makePage(1, {"Qt::All"}), 0).first().replacement == "Qt::ALL");
    assert(identifierMemory.suggestions(makePage(1, {"Qt::AIl"}), 0).isEmpty());

    auto split = WordEditor::apply(*first, {1}, {{"Q"}, {"Widget"}});
    assert(split && !isVerifiedCorrection(split->words[1]) && !isVerifiedCorrection(split->words[2]));
    auto merged = WordEditor::apply(*first, {0, 1}, {{"classQWidget"}});
    assert(merged && !isVerifiedCorrection(merged->words[0]));
    auto inserted = WordEditor::apply(*first, {1}, {{"QWidget"}, {"extra"}});
    assert(inserted && !isVerifiedCorrection(inserted->words[2]));
}

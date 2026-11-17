#include "documentcommands.h"
#include "wordeditor.h"

#include <QUndoStack>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

using namespace yagf::core;
using namespace yagf::ui;

int main()
{
    OcrDocument document;
    document.sourcePath = QStringLiteral("image.png");
    OcrPage source;
    source.number = 1;
    source.width = source.sourceWidth = 200;
    source.height = source.sourceHeight = 100;
    source.words.append({1, 1, 1, 1, QRect(10, 20, 30, 12),
                         QStringLiteral("mod"), QStringLiteral("mod"), 90});
    source.words.append({1, 1, 1, 1, QRect(41, 20, 70, 12),
                         QStringLiteral("ernization"), QStringLiteral("ernization"), 90});
    source.words.append({1, 2, 1, 1, QRect(120, 20, 30, 12),
                         QStringLiteral("Done"), QStringLiteral("Done"), 90});
    document.replacePage(std::move(source));
    QUndoStack stack;
    stack.setClean();
    auto merged = WordEditor::apply(*document.page(1), {0, 1},
        {{QStringLiteral("modernization")}});
    assert(merged);
    stack.push(new MergeWordsCommand(document, 1, *merged));
    assert(document.plainText() == QStringLiteral("modernization\n\nDone"));
    assert(!stack.isClean());
    stack.undo();
    assert(document.plainText() == QStringLiteral("mod ernization\n\nDone"));
    assert(stack.isClean());
    stack.redo();
    assert(document.page(1)->words.at(0).sourceWordIds.size() == 2);

    stack.push(new ChangeRegionTypeCommand(document, 1,
        document.page(1)->regions.at(0).id, QStringLiteral("heading")));
    assert(document.page(1)->regions.at(0).type == QStringLiteral("heading"));
    stack.push(new MoveRegionCommand(document, 1,
        document.page(1)->regions.at(0).id, QRect(5, 10, 110, 30)));
    assert(document.page(1)->regions.at(0).bounds == QRect(5, 10, 110, 30));
    stack.push(new ChangeReadingOrderCommand(document, 1, 1, -1));
    assert(document.plainText() == QStringLiteral("Done\n\nmodernization"));
    stack.undo();
    stack.undo();
    stack.undo();
    assert(document.page(1)->regions.at(0).type == QStringLiteral("text"));
    assert(document.page(1)->regions.at(0).bounds == QRect(10, 20, 101, 12));
    stack.undo();
    assert(stack.isClean());
    assert(document.page(1)->words.size() == 3);

    auto replacement = WordEditor::apply(*document.page(1), {0}, {{QStringLiteral("mode")}});
    assert(replacement);
    stack.push(new ReplaceTextCommand(document, 1, *replacement));
    assert(document.page(1)->words.at(0).text == QStringLiteral("mode"));
    stack.undo();
    assert(document.page(1)->words.at(0).text == QStringLiteral("mod"));
    OcrPage improved = *document.page(1);
    improved.preprocessing.autoContrast = true;
    stack.push(new ReplacePageCommand(document, 1, improved, QStringLiteral("Improve page")));
    assert(document.page(1)->preprocessing.autoContrast);
    stack.undo();
    assert(!document.page(1)->preprocessing.autoContrast);
}

#include "documentcommands.h"
#include "formulatext.h"

namespace yagf::ui {

ChangeNativeOcrModelCommand::ChangeNativeOcrModelCommand(core::OcrDocument &document, QString url)
    : QUndoCommand(QStringLiteral("Use native OCR model")),document(document),before(document.nativeOcrUrl),after(std::move(url)) {}
void ChangeNativeOcrModelCommand::redo() {document.nativeOcrUrl=after;}
void ChangeNativeOcrModelCommand::undo() {document.nativeOcrUrl=before;}
ChangeFormulaOutputCommand::ChangeFormulaOutputCommand(core::OcrDocument &document,bool unicode)
    : QUndoCommand(QStringLiteral("Change formula output")),document(document),beforeMode(document.unicodeFormulas),afterMode(unicode),before(document.pages),after(document.pages)
{ for (auto &page:after) core::setFormulaOutput(page,unicode); }
void ChangeFormulaOutputCommand::redo() { document.pages=after; document.unicodeFormulas=afterMode; }
void ChangeFormulaOutputCommand::undo() { document.pages=before; document.unicodeFormulas=beforeMode; }

ChangeSymbolRulesCommand::ChangeSymbolRulesCommand(core::OcrDocument &document, QVector<core::SymbolRule> after)
    : QUndoCommand(QStringLiteral("Edit symbol recognition rules")), document(document),
      before(document.symbolRules), after(std::move(after)) {}
void ChangeSymbolRulesCommand::redo() { document.symbolRules = after; }
void ChangeSymbolRulesCommand::undo() { document.symbolRules = before; }

namespace {

core::OcrPage updatedRegion(const core::OcrDocument &document, int pageNumber,
                            const QString &regionId, const QString &type, const QRect &bounds,
                            bool changeType)
{
    core::OcrPage result = *document.page(pageNumber);
    for (core::OcrRegion &region : result.regions)
        if (region.id == regionId) {
            if (changeType) region.type = type;
            else region.bounds = bounds;
            break;
        }
    return result;
}

core::OcrPage reordered(const core::OcrDocument &document, int pageNumber,
                        int regionIndex, int direction)
{
    core::OcrPage result = *document.page(pageNumber);
    result.moveRegion(regionIndex, direction);
    return result;
}

} // namespace

ChangeDocumentContextCommand::ChangeDocumentContextCommand(core::OcrDocument &document, core::DocumentContext after)
    : QUndoCommand(QStringLiteral("Change document context")), document(document),
      before(document.context), after(std::move(after)) {}

void ChangeDocumentContextCommand::redo() { document.context = after; }
void ChangeDocumentContextCommand::undo() { document.context = before; }

PageStateCommand::PageStateCommand(core::OcrDocument &document, int pageNumber,
                                   std::optional<core::OcrPage> after, const QString &label)
    : QUndoCommand(label), document(document), pageNumber(pageNumber), after(std::move(after))
{
    if (const auto *page = document.page(pageNumber)) before = *page;
}

void PageStateCommand::restore(const std::optional<core::OcrPage> &page)
{
    if (page) document.replacePage(*page);
    else document.removePage(pageNumber);
}

void PageStateCommand::redo() { restore(after); }
void PageStateCommand::undo() { restore(before); }

ReplacePageCommand::ReplacePageCommand(core::OcrDocument &document, int pageNumber,
                                       core::OcrPage after, const QString &label)
    : PageStateCommand(document, pageNumber, std::move(after), label) {}

CorrectSkewCommand::CorrectSkewCommand(core::OcrDocument &document, int pageNumber,
                                       core::OcrPage after, const QString &label)
    : PageStateCommand(document, pageNumber, std::move(after), label) {}

ReplaceTextCommand::ReplaceTextCommand(core::OcrDocument &document, int pageNumber, core::OcrPage after)
    : PageStateCommand(document, pageNumber, std::move(after), QStringLiteral("Replace text")) {}

SplitWordCommand::SplitWordCommand(core::OcrDocument &document, int pageNumber, core::OcrPage after)
    : PageStateCommand(document, pageNumber, std::move(after), QStringLiteral("Split word")) {}

MergeWordsCommand::MergeWordsCommand(core::OcrDocument &document, int pageNumber, core::OcrPage after)
    : PageStateCommand(document, pageNumber, std::move(after), QStringLiteral("Merge words")) {}

ChangeRegionTypeCommand::ChangeRegionTypeCommand(core::OcrDocument &document, int pageNumber,
                                                 const QString &regionId, const QString &type)
    : PageStateCommand(document, pageNumber,
          updatedRegion(document, pageNumber, regionId, type, {}, true),
          QStringLiteral("Change region type")) {}

MoveRegionCommand::MoveRegionCommand(core::OcrDocument &document, int pageNumber,
                                     const QString &regionId, const QRect &bounds)
    : PageStateCommand(document, pageNumber,
          updatedRegion(document, pageNumber, regionId, {}, bounds, false),
          QStringLiteral("Move region")) {}

ChangeReadingOrderCommand::ChangeReadingOrderCommand(core::OcrDocument &document, int pageNumber,
                                                     int regionIndex, int direction)
    : PageStateCommand(document, pageNumber,
          reordered(document, pageNumber, regionIndex, direction),
          QStringLiteral("Change reading order")) {}

ExtractRegionCommand::ExtractRegionCommand(core::OcrDocument &document, int pageNumber, core::OcrPage after)
    : PageStateCommand(document, pageNumber, std::move(after), QStringLiteral("Create region")) {}

} // namespace yagf::ui

#pragma once

#include "ocrdocument.h"

#include <QUndoCommand>

#include <optional>

namespace yagf::ui {

class ChangeDocumentContextCommand final : public QUndoCommand {
public:
    ChangeDocumentContextCommand(core::OcrDocument &document, core::DocumentContext after);
    void redo() override;
    void undo() override;
private:
    core::OcrDocument &document;
    core::DocumentContext before, after;
};

class PageStateCommand : public QUndoCommand {
public:
    void redo() override;
    void undo() override;
protected:
    PageStateCommand(core::OcrDocument &document, int pageNumber,
                     std::optional<core::OcrPage> after, const QString &label);
private:
    void restore(const std::optional<core::OcrPage> &page);
    core::OcrDocument &document;
    int pageNumber;
    std::optional<core::OcrPage> before;
    std::optional<core::OcrPage> after;
};

class ReplacePageCommand final : public PageStateCommand {
public:
    ReplacePageCommand(core::OcrDocument &document, int pageNumber,
                       core::OcrPage after, const QString &label);
};

class ReplaceTextCommand final : public PageStateCommand {
public:
    ReplaceTextCommand(core::OcrDocument &document, int pageNumber, core::OcrPage after);
};

class SplitWordCommand final : public PageStateCommand {
public:
    SplitWordCommand(core::OcrDocument &document, int pageNumber, core::OcrPage after);
};

class MergeWordsCommand final : public PageStateCommand {
public:
    MergeWordsCommand(core::OcrDocument &document, int pageNumber, core::OcrPage after);
};

class ChangeRegionTypeCommand final : public PageStateCommand {
public:
    ChangeRegionTypeCommand(core::OcrDocument &document, int pageNumber,
                            const QString &regionId, const QString &type);
};

class MoveRegionCommand final : public PageStateCommand {
public:
    MoveRegionCommand(core::OcrDocument &document, int pageNumber,
                      const QString &regionId, const QRect &bounds);
};

class ChangeReadingOrderCommand final : public PageStateCommand {
public:
    ChangeReadingOrderCommand(core::OcrDocument &document, int pageNumber,
                              int regionIndex, int direction);
};

class ExtractRegionCommand final : public PageStateCommand {
public:
    ExtractRegionCommand(core::OcrDocument &document, int pageNumber, core::OcrPage after);
};

} // namespace yagf::ui

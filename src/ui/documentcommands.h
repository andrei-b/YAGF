#pragma once

#include "ocrdocument.h"

#include <QUndoCommand>

#include <optional>
#include <functional>

namespace yagf::ui {

class ChangePageSequenceCommand final : public QUndoCommand {
public:
    ChangePageSequenceCommand(core::OcrDocument &document, core::OcrDocument after,
        int beforePage, int afterPage, std::function<void(int)> refresh, const QString &label);
    void redo() override;
    void undo() override;
private:
    core::OcrDocument &document;
    struct State {
        QVector<core::OcrPage> pages;
        QVector<core::DocumentPageSource> sources;
        int totalPages;
    };
    void restore(const State &state, int selectedPage);
    State before, after;
    int beforePage, afterPage;
    std::function<void(int)> refresh;
};

class ChangeNativeOcrModelCommand final : public QUndoCommand {
public:
    ChangeNativeOcrModelCommand(core::OcrDocument &document, QString url);
    void redo() override;
    void undo() override;
private:
    core::OcrDocument &document;
    QString before, after;
};

class ChangeFormulaOutputCommand final : public QUndoCommand {
public:
    ChangeFormulaOutputCommand(core::OcrDocument &document, bool unicode);
    void redo() override;
    void undo() override;
private:
    core::OcrDocument &document;
    bool beforeMode, afterMode;
    QVector<core::OcrPage> before, after;
};

class ChangeSymbolRulesCommand final : public QUndoCommand {
public:
    ChangeSymbolRulesCommand(core::OcrDocument &document, QVector<core::SymbolRule> after);
    void redo() override;
    void undo() override;
private:
    core::OcrDocument &document;
    QVector<core::SymbolRule> before, after;
};

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
    int affectedPage() const { return pageNumber; }
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

class CorrectSkewCommand final : public PageStateCommand {
public:
    CorrectSkewCommand(core::OcrDocument &document, int pageNumber,
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

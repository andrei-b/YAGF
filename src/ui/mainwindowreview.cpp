#include "mainwindow.h"
#include "aireviewdialog.h"
#include "experimentdialog.h"
#include "correctionretention.h"
#include "documentcommands.h"
#include "imageprocessor.h"
#include "pagesource.h"
#include "symbolrulesdialog.h"
#include <algorithm>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGraphicsScene>
#include <QImageReader>
#include <QLabel>
#include <QMessageBox>
#include <QTextEdit>
#include <QTimer>

namespace yagf::ui {
void MainWindow::editSymbolRules(bool fromSelection)
{
    if (processor.isBusy() || exportBusy || projectLoading || originalPath.isEmpty()) return;
    if (fromSelection && editorPending) { showError(tr("Apply your text edits, then select the printed symbol to teach.")); return; }
    if (!fromSelection && !maybeApplyEditor()) return;
    QImage seed;
    QRect bounds;
    QString reading;
    int wordIndex = -1, characterStart = -1, characterLength = 0;
    if (fromSelection) {
        const auto *page = document.page(currentPage);
        if (!page || selectedWords.isEmpty()) return;
        const auto cursor = selectionEditor->textCursor();
        if (cursor.hasSelection()) {
            for (auto it = editorWordRanges.cbegin(); it != editorWordRanges.cend(); ++it)
                if (cursor.selectionStart() >= it.value().first && cursor.selectionEnd() <= it.value().first+it.value().second) {
                    wordIndex = it.key(); characterStart = cursor.selectionStart()-it.value().first;
                    characterLength = cursor.selectionEnd()-cursor.selectionStart(); reading = cursor.selectedText(); break;
                }
        } else if (selectedWords.size() == 1) { wordIndex = selectedWords.first(); reading = page->words[wordIndex].text; }
        if (wordIndex < 0) { showError(tr("Select a symbol within one recognized word, or select a single word to crop its symbol.")); return; }
        seed = QImage(imagePath); bounds = page->words[wordIndex].bounds;
        if (seed.isNull() || bounds.isEmpty() || !seed.rect().contains(bounds)) { showError(tr("The selected text has no usable source image.")); return; }
    }
    auto shared = core::loadSymbolLibrary();
    if (!shared) { showError(shared.error()); return; }
    auto snapshot = document; snapshot.sourcePath = originalPath; snapshot.totalPages = pageCount;
    SymbolRulesDialog dialog(std::move(snapshot), *shared, this, seed, bounds, currentPage, reading, wordIndex, characterStart, characterLength);
    while (dialog.exec() == QDialog::Accepted) {
        const auto sharedRules = dialog.sharedRules();
        if (core::symbolRulesJson(sharedRules) != core::symbolRulesJson(*shared)) {
            if (auto saved = core::saveSymbolLibrary(sharedRules); !saved) { showError(saved.error()); continue; }
        }
        const auto rules = dialog.bookRules();
        const bool changed = core::symbolRulesJson(rules) != core::symbolRulesJson(document.symbolRules);
        const auto pages = dialog.changedPages();
        if (changed || !pages.isEmpty()) {
            undoStack.beginMacro(tr("Update symbol recognition"));
            if (changed) undoStack.push(new ChangeSymbolRulesCommand(document, rules));
            for (const auto &page : pages) undoStack.push(new ReplacePageCommand(document, page.number, page, tr("Apply symbol recognition")));
            undoStack.endMacro();
        }
        status->setText(tr("Symbol recognition rules saved. Trained, enabled rules will augment future OCR."));
        break;
    }
}

void MainWindow::openExperiments()
{
    if(processor.isBusy() || exportBusy || originalPath.isEmpty() || !maybeApplyEditor()) return;
    auto *dialog=new ExperimentDialog(document,this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
}

void MainWindow::refreshRenderedPageIfNeeded()
{
    if (processor.isBusy() || exportBusy || !processing::isPagedSource(originalPath)) return;
    const auto *page = document.page(currentPage);
    if (page && (page->sourceWidth != rawImage.width() || page->sourceHeight != rawImage.height())) showPage(currentPage);
}

void MainWindow::configureRendering()
{
    if (processor.isBusy() || exportBusy || !processing::isPagedSource(originalPath) || !maybeApplyEditor()) return;
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Rendering resolution"));
    auto *layout = new QFormLayout(&dialog);
    auto *resolution = new QComboBox;
    for (int dpi : {150, 200, 300, 400}) resolution->addItem(tr("%1 DPI").arg(dpi), dpi);
    resolution->addItem(tr("Adaptive (150–400 DPI)"), -1);
    resolution->setObjectName(QStringLiteral("renderDpi"));
    resolution->setCurrentIndex(document.rendering.adaptive ? 4 : resolution->findData(document.rendering.dpi));
    layout->addRow(tr("New pages:"), resolution);
    auto *explanation = new QLabel(tr("Adaptive mode measures text on a 150 DPI preview and increases resolution for small characters. Existing pages keep their saved DPI until explicitly re-rendered."));
    explanation->setWordWrap(true); layout->addRow(explanation);
    auto *applyCurrent = new QCheckBox(tr("Re-render the current page and scale its saved boxes"));
    applyCurrent->setObjectName(QStringLiteral("applyRenderDpiToCurrent"));
    layout->addRow(applyCurrent);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    layout->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    const int dpi = resolution->currentData().toInt();
    const core::RenderSettings settings{dpi < 0 ? 150 : dpi, dpi < 0};
    if (document.rendering != settings) { document.rendering = settings; markDirty(); }
    if (applyCurrent->isChecked()) processor.renderDocumentPage(currentPage, dpi);
}

void MainWindow::activateVerification(core::VerificationItem item, int action)
{
    if (processor.isBusy() || exportBusy || !maybeApplyEditor()) return;
    verificationTarget = std::move(item); verificationAction = action;
    if (verificationTarget->page != currentPage || imagePath.isEmpty()) showPage(verificationTarget->page);
    else finishVerification();
}

void MainWindow::finishVerification()
{
    if (!verificationTarget || verificationTarget->page != currentPage || processor.isBusy() || imagePath.isEmpty()) return;
    const auto item = *verificationTarget;
    const int action = verificationAction;
    verificationTarget.reset();
    const auto *page = document.page(currentPage);
    if (!page || item.wordId.isEmpty()) return;
    if (!item.pending) {
        const auto indices = indicesForIds({item.wordId});
        if (indices.isEmpty()) return;
        selectWord(indices.first());
        if (action == 1) reviewSelectedWord();
        else if (action == 2) {
            auto edited = *page;
            edited.words[indices.first()].reviewed = true;
            edited.words[indices.first()].alternative = {};
            undoStack.push(new ReplacePageCommand(document, currentPage, std::move(edited), tr("Verify word and source box")));
        }
        return;
    }
    const auto saved = std::find_if(page->pendingCorrections.begin(), page->pendingCorrections.end(), [&](const auto &word) { return word.id == item.wordId; });
    if (saved == page->pendingCorrections.end()) return;
    const auto correction = *saved;
    updateSelection({});
    view->centerOn(correction.bounds.center());
    status->setText(tr("Saved correction: %1 · Review to restore, or discard explicitly.").arg(item.text.left(120)));
    if (!action) return;
    if (action == 3) {
        if (QMessageBox::question(this, tr("Discard saved correction"), tr("Discard this retained correction? This can be undone.")) != QMessageBox::Yes) return;
        auto edited = *page;
        edited.pendingCorrections.removeIf([&](const auto &word) { return word.id == item.wordId; });
        // A discarded deletion must not reappear on the next recognition run.
        edited.deletedWords.removeIf([&](const auto &word) { return word.id == item.wordId; });
        undoStack.push(new ReplacePageCommand(document, currentPage, std::move(edited), tr("Discard saved correction")));
        return;
    }
    if (action != 1) return;
    QVector<int> span;
    for (int i = 0; i < page->words.size(); ++i)
        if (page->words[i].bounds.intersects(correction.bounds)) span.append(i);
    for (int i = 1; i < span.size(); ++i) if (span[i] != span[i - 1] + 1) {
        showError(tr("The saved correction overlaps separate passages. Adjust the blocks or current text before restoring it.")); return;
    }
    QStringList current;
    for (int i : span) current.append(page->words[i].text);
    QString reading = correction.text;
    if (reading.isEmpty()) {
        if (QMessageBox::question(this, tr("Restore saved deletion"), tr("Delete the current overlapping text?\n%1").arg(current.join(' '))) != QMessageBox::Yes) return;
    } else {
        QImage image(imagePath);
        const QRect crop = correction.bounds.adjusted(-40, -30, 40, 30).intersected(image.rect());
        AiReviewDialog dialog(reading, tr("Current OCR to replace: %1").arg(current.isEmpty() ? tr("(no overlapping text)") : current.join(' ')),
                              image.copy(crop), correction.bounds.translated(-crop.topLeft()), aiReviewToken, this, nullptr, true);
        dialog.setDocumentContext(core::reconstructionContext(document, currentPage, span.isEmpty() ? 0 : span.first()));
        if (dialog.exec() != QDialog::Accepted) return;
        aiReviewToken = dialog.sessionToken(); reading = dialog.correction();
    }
    auto edited = core::restorePendingCorrection(*page, item.wordId, reading, span);
    if (!edited) { showError(edited.error()); return; }
    undoStack.push(new ReplacePageCommand(document, currentPage, std::move(*edited), tr("Restore saved correction")));
}
}

#include "mainwindow.h"
#include "aireviewdialog.h"
#include "correctionretention.h"
#include "documentcommands.h"
#include "imageprocessor.h"
#include "pagesource.h"
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

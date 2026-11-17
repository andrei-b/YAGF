#include "mainwindow.h"
#include "projectfile.h"
#include "preprocessdialog.h"
#include "imageprocessor.h"
#include "layoutdialog.h"
#include "scannerdialog.h"
#include "documentcommands.h"
#include "wordeditor.h"

#include <QCloseEvent>
#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QActionGroup>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGraphicsPixmapItem>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QHBoxLayout>
#include <QImageReader>
#include <QImageIOHandler>
#include <QIcon>
#include <QJsonDocument>
#include <QMetaObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMouseEvent>
#include <QMenuBar>
#include <QMimeData>
#include <QRegularExpression>
#include <QSet>
#include <QRubberBand>
#include <QTextEdit>
#include <QTextCursor>
#include <QTextCharFormat>
#include <QResizeEvent>
#include <QWheelEvent>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSaveFile>
#include <QSplitter>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStatusBar>
#include <QToolBar>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <QUuid>
#include <QUrl>

#include <utility>
#include <cmath>
#include <algorithm>
#include <limits>

namespace yagf::ui {
using core::OcrDocument;
using core::OcrPage;
using core::OcrRegion;
using core::OcrWord;
using core::ProjectFile;
using processing::DocumentProcessor;
using processing::ImageProcessor;
using processing::BatchPageOptions;

namespace {
bool overlapsWord(const QRect &box, const QRect &selection)
{
    if (selection.contains(box.center())) return true;
    const QRect overlap = box.intersected(selection);
    return !overlap.isEmpty() &&
        static_cast<qint64>(overlap.width()) * overlap.height() * 4 >=
        static_cast<qint64>(box.width()) * box.height();
}

void carryCorrections(OcrPage &fresh, const OcrPage *oldPage)
{
    if (!oldPage) return;
    fresh.regions = oldPage->regions;
    for (OcrWord &word : fresh.words) {
        for (const OcrWord &old : oldPage->words) {
            if (old.bounds != word.bounds || old.originalText != word.originalText) continue;
            word.id = old.id;
            word.bold = old.bold;
            word.italic = old.italic;
            word.sourceWordIds = old.sourceWordIds;
            word.fontSizePt = old.fontSizePt;
            word.sourceFontSizePt = old.sourceFontSizePt;
            if (old.reviewed) { word.text = old.text; word.reviewed = true; }
            break;
        }
    }
    if (oldPage->regions.size() < 2) return;
    QHash<QString, int> oldOrder;
    for (int i = 0; i < oldPage->regions.size(); ++i)
        oldOrder.insert(oldPage->regions.at(i).id, i);
    QVector<QVector<OcrWord>> groups;
    for (const OcrWord &word : fresh.words) {
        if (groups.isEmpty() || groups.last().last().block != word.block) groups.emplaceBack();
        groups.last().append(word);
    }
    const auto orderOf = [&oldOrder, &fresh](const QVector<OcrWord> &group) {
        return oldOrder.value(QStringLiteral("page-%1/region-%2")
            .arg(fresh.number).arg(group.first().block), std::numeric_limits<int>::max());
    };
    std::stable_sort(groups.begin(), groups.end(), [&](const auto &a, const auto &b) {
        return orderOf(a) < orderOf(b);
    });
    fresh.words.clear();
    for (const auto &group : groups) fresh.words += group;
}
}

PageView::PageView(QWidget *parent) : QGraphicsView(parent), rubberBand(new QRubberBand(QRubberBand::Rectangle, viewport())) {}

void PageView::setRegionMode(bool enabled)
{
    regionMode = enabled;
    rubberBand->hide();
    setDragMode(enabled ? QGraphicsView::NoDrag : QGraphicsView::ScrollHandDrag);
    viewport()->setCursor(enabled ? Qt::CrossCursor : Qt::ArrowCursor);
}

void PageView::fitPage()
{
    fitMode = true;
    if (scene() && !scene()->sceneRect().isEmpty())
        fitInView(scene()->sceneRect(), Qt::KeepAspectRatio);
}

void PageView::zoomBy(qreal factor)
{
    const qreal next = transform().m11() * factor;
    if (next < 0.05 || next > 8.0) return;
    fitMode = false;
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    scale(factor, factor);
}

void PageView::mousePressEvent(QMouseEvent *event)
{
    if (auto *box = qgraphicsitem_cast<QGraphicsRectItem *>(itemAt(event->pos()))) {
        const QVariant index = box->data(0);
        if (index.isValid())
            emit wordClicked(index.toInt());
    }
    if (regionMode && event->button() == Qt::LeftButton) {
        regionOrigin = event->pos();
        rubberBand->setGeometry(QRect(regionOrigin, QSize()));
        rubberBand->show();
        event->accept();
        return;
    }
    QGraphicsView::mousePressEvent(event);
}

void PageView::mouseMoveEvent(QMouseEvent *event)
{
    if (regionMode && rubberBand->isVisible()) {
        rubberBand->setGeometry(QRect(regionOrigin, event->pos()).normalized());
        event->accept();
        return;
    }
    QGraphicsView::mouseMoveEvent(event);
}

void PageView::mouseReleaseEvent(QMouseEvent *event)
{
    if (regionMode && event->button() == Qt::LeftButton && rubberBand->isVisible()) {
        const QRect viewportRect = rubberBand->geometry();
        rubberBand->hide();
        if (viewportRect.width() >= 6 && viewportRect.height() >= 6) {
            const QRectF sceneRect(mapToScene(viewportRect.topLeft()), mapToScene(viewportRect.bottomRight()));
            emit regionSelected(sceneRect.normalized());
        }
        event->accept();
        return;
    }
    QGraphicsView::mouseReleaseEvent(event);
}

void PageView::resizeEvent(QResizeEvent *event)
{
    QGraphicsView::resizeEvent(event);
    if (fitMode) fitPage();
}

void PageView::wheelEvent(QWheelEvent *event)
{
    if (event->modifiers().testFlag(Qt::ControlModifier)) {
        zoomBy(event->angleDelta().y() > 0 ? 1.2 : 1.0 / 1.2);
        event->accept();
        return;
    }
    QGraphicsView::wheelEvent(event);
}

void PageView::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasImage() || event->mimeData()->hasUrls())
        event->acceptProposedAction();
    else QGraphicsView::dragEnterEvent(event);
}

void PageView::dropEvent(QDropEvent *event)
{
    if (event->mimeData()->hasImage() || event->mimeData()->hasUrls()) {
        emit inputDropped(event->mimeData());
        event->acceptProposedAction();
    } else QGraphicsView::dropEvent(event);
}

MainWindow::MainWindow()
{
    updateTitle();
    resize(1200, 800);
    auto *fileMenu = menuBar()->addMenu(tr("&File"));
    auto *editMenu = menuBar()->addMenu(tr("&Edit"));
    auto *toolsMenu = menuBar()->addMenu(tr("&Tools"));
    undoAction = editMenu->addAction(tr("Undo"), this, [this] {
        if (!processor.isBusy() && !exportBusy && maybeApplyEditor()) undoStack.undo();
    });
    undoAction->setShortcut(QKeySequence::Undo);
    redoAction = editMenu->addAction(tr("Redo"), this, [this] {
        if (!processor.isBusy() && !exportBusy && maybeApplyEditor()) undoStack.redo();
    });
    redoAction->setShortcut(QKeySequence::Redo);
    undoAction->setEnabled(false);
    redoAction->setEnabled(false);
    auto *toolbar = addToolBar(tr("Document"));
    fileMenu->addAction(toolbar->addAction(tr("Open image or PDF"), this, &MainWindow::openFile));
    fileMenu->addAction(tr("Scan a page…"), this, &MainWindow::scanPage);
    auto *pasteAction = fileMenu->addAction(tr("Paste image from clipboard"), this, &MainWindow::pasteImage);
    pasteAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+V")));
    fileMenu->addAction(toolbar->addAction(tr("Open project"), this, &MainWindow::openProject));
    fileMenu->addAction(toolbar->addAction(tr("Save project"), this, &MainWindow::saveProject));
    fileMenu->addAction(tr("Save project as…"), this, &MainWindow::saveProjectAs);
    toolbar->addSeparator();
    previousButton = new QPushButton(QIcon(QStringLiteral(":/legacy/back.png")), {}, this);
    previousButton->setToolTip(tr("Previous page"));
    nextButton = new QPushButton(QIcon(QStringLiteral(":/legacy/forward.png")), {}, this);
    nextButton->setToolTip(tr("Next page"));
    previousButton->setIconSize(QSize(24, 24));
    nextButton->setIconSize(QSize(24, 24));
    pageSpin = new QSpinBox(this);
    pageSpin->setRange(1, 1);
    pageSpin->setToolTip(tr("PDF page number"));
    pageSpin->setMaximumWidth(72);
    toolbar->addWidget(previousButton);
    toolbar->addWidget(pageSpin);
    toolbar->addWidget(nextButton);
    connect(previousButton, &QPushButton::clicked, this, [this] { showPage(currentPage - 1); });
    connect(nextButton, &QPushButton::clicked, this, [this] { showPage(currentPage + 1); });
    connect(pageSpin, &QSpinBox::valueChanged, this, &MainWindow::showPage);
    updatePageControls();
    toolbar->addSeparator();
    auto *modeGroup = new QActionGroup(this);
    auto *panAction = toolbar->addAction(tr("Pan"));
    panAction->setCheckable(true);
    panAction->setChecked(true);
    modeGroup->addAction(panAction);
    auto *regionAction = toolbar->addAction(tr("Select region"));
    regionAction->setCheckable(true);
    modeGroup->addAction(regionAction);
    connect(regionAction, &QAction::toggled, this, [this](bool enabled) { view->setRegionMode(enabled); });
    toolbar->addAction(QIcon(QStringLiteral(":/legacy/smaller.png")), tr("Zoom out"), this,
                       [this] { view->zoomBy(1.0 / 1.25); });
    toolbar->addAction(tr("Fit page"), this, [this] { view->fitPage(); });
    toolbar->addAction(QIcon(QStringLiteral(":/legacy/larger.png")), tr("Zoom in"), this,
                       [this] { view->zoomBy(1.25); });
    toolbar->addSeparator();
    improveAction = toolbar->addAction(tr("Improve page…"), this, &MainWindow::improveCurrentPage);
    improveAction->setObjectName(QStringLiteral("improvePageAction"));
    improveAction->setEnabled(false);
    toolsMenu->addAction(improveAction);
    layoutAction = toolsMenu->addAction(tr("Edit layout…"), this, &MainWindow::editLayout);
    createRegionAction = toolsMenu->addAction(tr("Create region from selection"), this,
                                             &MainWindow::createRegionFromSelection);
    batchAction = toolsMenu->addAction(tr("Recognize all pages"), this, &MainWindow::runBatchOcr);
    layoutAction->setEnabled(false);
    createRegionAction->setEnabled(false);
    batchAction->setEnabled(false);
    ocrButton = new QPushButton(tr("Recognize"), this);
    toolbar->addWidget(ocrButton);
    connect(ocrButton, &QPushButton::clicked, this, &MainWindow::runOcr);
    cancelButton = new QPushButton(tr("Cancel"), this);
    cancelButton->setEnabled(false);
    toolbar->addWidget(cancelButton);
    connect(cancelButton, &QPushButton::clicked, &processor, &DocumentProcessor::cancel);
    connect(cancelButton, &QPushButton::clicked, this, [this] {
        if (exportBusy && pdfCancel) {
            pdfCancel->store(true);
            cancelButton->setEnabled(false);
            status->setText(tr("Cancelling PDF export…"));
        }
    });
    toolbar->addSeparator();
    toolbar->addWidget(new QLabel(tr("Language: "), this));
    languageEdit = new QLineEdit(QStringLiteral("eng"), this);
    languageEdit->setToolTip(tr("Tesseract language code, for example eng or eng+deu"));
    languageEdit->setMaximumWidth(120);
    toolbar->addWidget(languageEdit);
    toolbar->addSeparator();
    fileMenu->addSeparator();
    fileMenu->addAction(tr("Export text"), this, &MainWindow::exportText);
    fileMenu->addAction(tr("Export layout HTML"), this, &MainWindow::exportHtml);
    fileMenu->addAction(tr("Export JSON"), this, &MainWindow::exportJson);
    fileMenu->addSeparator();
    pdfAction = fileMenu->addAction(tr("Export searchable PDF…"), this, [this] { exportPdf(false); });
    pdfaAction = fileMenu->addAction(tr("Export archival PDF/A…"), this, [this] { exportPdf(true); });
    pdfAction->setEnabled(false);
    pdfaAction->setEnabled(false);

    auto *splitter = new QSplitter(this);
    pageList = new QListWidget(splitter);
    pageList->setObjectName(QStringLiteral("pageMiniatures"));
    pageList->setViewMode(QListView::IconMode);
    pageList->setFlow(QListView::TopToBottom);
    pageList->setWrapping(false);
    pageList->setMovement(QListView::Static);
    pageList->setResizeMode(QListView::Adjust);
    pageList->setGridSize(QSize(146, 190));
    pageList->setIconSize(QSize(118, 156));
    pageList->setMinimumWidth(150);
    pageList->setMaximumWidth(185);
    pageList->setToolTip(tr("Select a page miniature to preview it."));
    view = new PageView(splitter);
    view->setObjectName(QStringLiteral("pagePreview"));
    scene = new QGraphicsScene(this);
    view->setScene(scene);
    view->setAcceptDrops(true);
    view->setRegionMode(false);
    view->setRenderHint(QPainter::Antialiasing);
    view->setBackgroundBrush(QColor(224, 228, 233));
    auto *textPanel = new QWidget(splitter);
    auto *textLayout = new QVBoxLayout(textPanel);
    textLayout->setContentsMargins(6, 6, 6, 6);
    selectionLabel = new QLabel(tr("Recognized text"), textPanel);
    textLayout->addWidget(selectionLabel);
    auto *formatToolbar = new QToolBar(textPanel);
    formatToolbar->setMovable(false);
    textLayout->addWidget(formatToolbar);
    selectionEditor = new QTextEdit(textPanel);
    selectionEditor->setObjectName(QStringLiteral("recognizedTextEditor"));
    selectionEditor->setPlaceholderText(tr("Select words on the page or drag a region, then edit here."));
    selectionEditor->setAcceptRichText(true);
    selectionEditor->setMinimumHeight(180);
    textLayout->addWidget(selectionEditor, 2);
    auto *editButtons = new QHBoxLayout();
    applyButton = new QPushButton(tr("Apply correction"), textPanel);
    applyButton->setEnabled(false);
    regionOcrButton = new QPushButton(tr("Recognize region"), textPanel);
    regionOcrButton->setObjectName(QStringLiteral("recognizeRegionButton"));
    regionOcrButton->setEnabled(false);
    editButtons->addWidget(applyButton);
    editButtons->addWidget(regionOcrButton);
    textLayout->addLayout(editButtons);
    textLayout->addWidget(new QLabel(tr("Words on this page"), textPanel));
    wordList = new QListWidget(textPanel);
    wordList->setObjectName(QStringLiteral("recognizedWords"));
    wordList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    wordList->setToolTip(tr("Select a word to locate it on the scan and edit it above."));
    textLayout->addWidget(wordList, 3);
    auto *boldAction = formatToolbar->addAction(tr("Bold"));
    boldAction->setShortcut(QKeySequence::Bold);
    boldAction->setToolTip(tr("Format selected editor text as bold"));
    connect(boldAction, &QAction::triggered, this, [this] {
        QTextCharFormat format;
        const bool bold = selectionEditor->currentCharFormat().fontWeight() >= QFont::Bold;
        format.setFontWeight(bold ? QFont::Normal : QFont::Bold);
        selectionEditor->mergeCurrentCharFormat(format);
        editorPending = true;
    });
    auto *italicAction = formatToolbar->addAction(tr("Italic"));
    italicAction->setShortcut(QKeySequence::Italic);
    italicAction->setToolTip(tr("Format selected editor text as italic"));
    connect(italicAction, &QAction::triggered, this, [this] {
        QTextCharFormat format;
        format.setFontItalic(!selectionEditor->currentCharFormat().fontItalic());
        selectionEditor->mergeCurrentCharFormat(format);
        editorPending = true;
    });
    splitter->addWidget(pageList);
    splitter->addWidget(view);
    splitter->addWidget(textPanel);
    splitter->setStretchFactor(1, 3);
    splitter->setStretchFactor(2, 1);
    splitter->setSizes({170, 710, 320});
    splitter->setChildrenCollapsible(false);
    setCentralWidget(splitter);
    setAcceptDrops(true);
    connect(view, &PageView::inputDropped, this, &MainWindow::importMimeData);
    updatePageControls();
    connect(pageList, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row >= 0) showPage(row + 1);
    });
    connect(view, &PageView::regionSelected, this, &MainWindow::selectRegion);
    connect(applyButton, &QPushButton::clicked, this, &MainWindow::applyCorrection);
    connect(regionOcrButton, &QPushButton::clicked, this, &MainWindow::runRegionOcr);
    connect(selectionEditor, &QTextEdit::textChanged, this, [this] { editorPending = true; });
    connect(view, &PageView::wordClicked, this, &MainWindow::selectWord);
    connect(wordList, &QListWidget::itemSelectionChanged, this, [this] {
        QStringList ids;
        for (QListWidgetItem *item : wordList->selectedItems()) {
            const int row = wordList->row(item);
            if (row >= 0 && row < displayedWords.size()) ids.append(displayedWords.at(row)->id);
        }
        if (!maybeApplyEditor()) { updateSelection(selectedWords); return; }
        updateSelection(indicesForIds(ids));
    });
    status = new QLabel(tr("Open an image or PDF to begin."), this);
    statusBar()->addPermanentWidget(status);

    connect(&processor, &DocumentProcessor::busyChanged, this, &MainWindow::setBusy);
    connect(&processor, &DocumentProcessor::failed, this, &MainWindow::showError);
    connect(&processor, &DocumentProcessor::cancelled, this, [this] { status->setText(tr("Cancelled")); });
    connect(&processor, &DocumentProcessor::pdfOpened, this, [this](int count) {
        if (!projectPath.isEmpty() && document.totalPages != count) {
            // Version 1 projects described only the first rendered page.
            if (document.totalPages == 1 && document.pages.size() == 1 && document.pages.first().number == 1)
                markDirty();
            else {
                showError(tr("The source PDF page count changed since this project was saved."));
                return;
            }
        }
        pageCount = count;
        document.totalPages = count;
        processor.startThumbnails(count);
        showPage(1);
    });
    connect(&processor, &DocumentProcessor::thumbnailReady, this, [this](int number, const QString &path) {
        if (number < 1 || number > pageList->count()) return;
        QImage miniature(path);
        if (const OcrPage *page = document.page(number); page && !page->preprocessing.isIdentity()) {
            const auto processed = ImageProcessor::apply(miniature, page->preprocessing);
            if (processed) miniature = *processed;
        }
        if (!miniature.isNull())
            pageList->item(number - 1)->setIcon(QIcon(QPixmap::fromImage(miniature).scaled(
                pageList->iconSize(), Qt::KeepAspectRatio, Qt::SmoothTransformation)));
    });
    connect(&processor, &DocumentProcessor::pageRendered, this, [this](int number, const QString &path) {
        if (number == currentPage) loadImage(path);
    });
    connect(&processor, &DocumentProcessor::recognized, this,
            [this](OcrPage newPage, const QPoint &offset, const QRect &region) {
        QImageReader reader(imagePath);
        const QSize size = reader.size();
        const OcrPage *oldPage = document.page(currentPage);
        if (oldPage) {
            newPage.sourceWidth = oldPage->sourceWidth;
            newPage.sourceHeight = oldPage->sourceHeight;
            newPage.preprocessing = oldPage->preprocessing;
        } else {
            newPage.sourceWidth = rawImage.width();
            newPage.sourceHeight = rawImage.height();
        }
        for (OcrWord &word : newPage.words) {
            word.bounds.translate(offset);
            if (!region.isEmpty()) { word.id.clear(); word.sourceWordIds.clear(); }
        }
        if (region.isEmpty()) carryCorrections(newPage, oldPage);
        else if (oldPage) {
            for (OcrWord &word : newPage.words) {
                for (const OcrWord &old : oldPage->words) {
                    if (old.bounds == word.bounds && old.originalText == word.originalText) {
                        word.id = old.id;
                        word.bold = old.bold;
                        word.italic = old.italic;
                        word.sourceWordIds = old.sourceWordIds;
                        word.fontSizePt = old.fontSizePt;
                        word.sourceFontSizePt = old.sourceFontSizePt;
                        if (old.reviewed) { word.text = old.text; word.reviewed = true; }
                        break;
                    }
                }
            }
        }
        document.sourcePath = originalPath;
        document.engine = newPage.engineId;
        document.language = newPage.language;
        document.totalPages = pageCount;
        if (region.isEmpty()) {
            undoStack.push(new ReplacePageCommand(document, currentPage, std::move(newPage), tr("Recognize page")));
        } else {
            OcrPage merged;
            if (oldPage) merged = *oldPage;
            merged.id = QStringLiteral("page-%1").arg(currentPage);
            merged.number = currentPage;
            merged.width = size.width();
            merged.height = size.height();
            if (!oldPage) {
                merged.sourceWidth = rawImage.width();
                merged.sourceHeight = rawImage.height();
            }
            merged.engineId = newPage.engineId;
            merged.language = newPage.language;
            int maxBlock = 0;
            for (const OcrWord &old : merged.words) maxBlock = qMax(maxBlock, old.block);
            for (OcrWord &word : newPage.words) {
                word.block += maxBlock;
            }
            QVector<OcrWord> kept;
            int insertion = -1;
            for (const OcrWord &old : merged.words) {
                if (overlapsWord(old.bounds, region)) {
                    if (insertion < 0) insertion = kept.size();
                } else {
                    kept.append(old);
                }
            }
            if (insertion < 0) {
                insertion = 0;
                while (insertion < kept.size() && kept.at(insertion).bounds.top() < region.top()) ++insertion;
            }
            for (const OcrWord &word : newPage.words) kept.insert(insertion++, word);
            merged.words = std::move(kept);
            undoStack.push(new ReplacePageCommand(document, currentPage, std::move(merged), tr("Recognize region")));
        }
        showDocument();
        if (!region.isEmpty()) selectRegion(region);
        status->setText(tr("Page %1: %2 words recognized").arg(currentPage).arg(document.page(currentPage)->words.size()));
    });
    connect(&processor, &DocumentProcessor::batchPageRecognized, this,
            [this](OcrPage page, const QSize &sourceSize) {
        const int number = page.number;
        const OcrPage *oldPage = document.page(number);
        if (oldPage && (oldPage->sourceWidth != sourceSize.width() ||
                        oldPage->sourceHeight != sourceSize.height() ||
                        oldPage->width != page.width || oldPage->height != page.height)) {
            processor.cancel();
            showError(tr("Page %1 no longer matches its saved source or processing geometry.").arg(number));
            return;
        }
        page.sourceWidth = sourceSize.width();
        page.sourceHeight = sourceSize.height();
        if (oldPage) page.preprocessing = oldPage->preprocessing;
        carryCorrections(page, oldPage);
        const QString language = page.language;
        const QString engineId = page.engineId;
        undoStack.push(new ReplacePageCommand(document, number, std::move(page), tr("Recognize page")));
        document.engine = engineId;
        document.language = language;
        document.totalPages = pageCount;
        if (number == currentPage) showDocument();
    });
    connect(&processor, &DocumentProcessor::batchProgress, this, [this](int completed, int total) {
        status->setText(tr("Batch OCR: %1 of %2 pages complete").arg(completed).arg(total));
    });
    connect(&processor, &DocumentProcessor::batchFinished, this, [this] {
        status->setText(tr("Batch OCR complete: %1 pages").arg(pageCount));
    });
    connect(&pdfWatcher, &QFutureWatcher<processing::PdfExportResult>::finished, this, [this] {
        const auto result = pdfWatcher.result();
        exportBusy = false;
        pdfCancel.reset();
        setBusy(processor.isBusy());
        if (result.status == processing::PdfExportResult::Status::Success)
            status->setText(tr("PDF exported: %1").arg(pdfExportPath));
        else if (result.status == processing::PdfExportResult::Status::Cancelled)
            status->setText(tr("PDF export cancelled"));
        else showError(result.error);
    });
    connect(&undoStack, &QUndoStack::indexChanged, this, [this] {
        const OcrPage *page = document.page(currentPage);
        if (page && !rawImagePath.isEmpty() && page->preprocessing != displayedPreprocessing)
            loadImage(rawImagePath);
        else showDocument();
        refreshDirty();
        undoAction->setEnabled(!processor.isBusy() && !exportBusy && undoStack.canUndo());
        redoAction->setEnabled(!processor.isBusy() && !exportBusy && undoStack.canRedo());
    });
}

MainWindow::~MainWindow()
{
    if (exportBusy && pdfCancel) {
        pdfCancel->store(true);
        pdfWatcher.waitForFinished();
    }
}

void MainWindow::setBusy(bool busy)
{
    const bool blocked = busy || exportBusy;
    undoAction->setEnabled(!blocked && undoStack.canUndo());
    redoAction->setEnabled(!blocked && undoStack.canRedo());
    ocrButton->setEnabled(!blocked && !imagePath.isEmpty());
    cancelButton->setEnabled(blocked);
    selectionEditor->setEnabled(!blocked);
    applyButton->setEnabled(!blocked && !selectedWords.isEmpty());
    regionOcrButton->setEnabled(!blocked && !imagePath.isEmpty() && !selectedRegion.isEmpty());
    improveAction->setEnabled(!blocked && !rawImage.isNull());
    batchAction->setEnabled(!blocked && pageCount > 1 && originalPath.endsWith(QStringLiteral(".pdf"), Qt::CaseInsensitive));
    layoutAction->setEnabled(!blocked && document.page(currentPage) && !document.page(currentPage)->regions.isEmpty());
    createRegionAction->setEnabled(!blocked && !selectedWords.isEmpty() &&
        document.page(currentPage) && selectedWords.size() < document.page(currentPage)->words.size());
    pdfAction->setEnabled(!blocked && document.wordCount() > 0);
    pdfaAction->setEnabled(!blocked && document.wordCount() > 0);
    updatePageControls();
}

void MainWindow::updatePageControls()
{
    const QSignalBlocker blocker(pageSpin);
    pageSpin->setMaximum(pageCount);
    pageSpin->setValue(currentPage);
    const bool available = !processor.isBusy() && !exportBusy;
    previousButton->setEnabled(available && currentPage > 1);
    nextButton->setEnabled(available && currentPage < pageCount);
    pageSpin->setEnabled(available && pageCount > 1);
    if (pageList) {
        const QSignalBlocker listBlocker(pageList);
        if (pageList->count() != pageCount) {
            pageList->clear();
            for (int number = 1; number <= pageCount; ++number)
                pageList->addItem(tr("Page %1").arg(number));
        }
        pageList->setCurrentRow(currentPage - 1);
        pageList->setEnabled(available);
    }
    if (batchAction) batchAction->setEnabled(available && pageCount > 1 &&
        originalPath.endsWith(QStringLiteral(".pdf"), Qt::CaseInsensitive));
    if (layoutAction) layoutAction->setEnabled(available && document.page(currentPage) &&
        !document.page(currentPage)->regions.isEmpty());
    if (createRegionAction) createRegionAction->setEnabled(available && !selectedWords.isEmpty() &&
        document.page(currentPage) && selectedWords.size() < document.page(currentPage)->words.size());
}

void MainWindow::showError(const QString &message)
{
    status->setText(message);
    QMessageBox::warning(this, tr("YAGF Revival"), message);
}

void MainWindow::openFile()
{
    const QString path = QFileDialog::getOpenFileName(this, tr("Open document"), {},
        tr("Documents (*.pdf *.png *.jpg *.jpeg *.tif *.tiff *.bmp *.webp *.avif *.heic);;All files (*)"));
    loadSource(path);
}

void MainWindow::scanPage()
{
    if (processor.isBusy() || exportBusy) return;
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/imports");
    if (!QDir().mkpath(directory)) { showError(tr("Could not create the image import folder.")); return; }
    const QString path = directory + QLatin1Char('/') +
        QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(".png");
    ScannerDialog dialog(path, this);
    if (dialog.exec() != QDialog::Accepted) return;
    loadSource(path);
    if (originalPath != path) QFile::remove(path);
}

void MainWindow::pasteImage()
{
    importMimeData(QApplication::clipboard()->mimeData());
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasImage() || event->mimeData()->hasUrls())
        event->acceptProposedAction();
    else QMainWindow::dragEnterEvent(event);
}

void MainWindow::dropEvent(QDropEvent *event)
{
    if (event->mimeData()->hasImage() || event->mimeData()->hasUrls()) {
        importMimeData(event->mimeData());
        event->acceptProposedAction();
    } else QMainWindow::dropEvent(event);
}

void MainWindow::importMimeData(const QMimeData *mime)
{
    if (!mime || processor.isBusy() || exportBusy) return;
    if (mime->hasUrls()) {
        for (const QUrl &url : mime->urls()) {
            if (url.isLocalFile()) { loadSource(url.toLocalFile()); return; }
        }
    }
    if (mime->hasImage()) {
        const QImage image = qvariant_cast<QImage>(mime->imageData());
        if (image.isNull()) { showError(tr("The clipboard image could not be read.")); return; }
        const QString path = saveImportedImage(image);
        if (path.isEmpty()) return;
        loadSource(path);
        if (originalPath != path) QFile::remove(path);
        return;
    }
    showError(tr("Drop a local image or PDF, or paste an image from the clipboard."));
}

QString MainWindow::saveImportedImage(const QImage &image)
{
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/imports");
    if (!QDir().mkpath(directory)) { showError(tr("Could not create the image import folder.")); return {}; }
    const QString path = directory + QLatin1Char('/') +
        QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(".png");
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || !image.save(&file, "PNG") || !file.commit()) {
        file.cancelWriting();
        showError(tr("Could not save the imported image."));
        return {};
    }
    return path;
}

void MainWindow::loadSource(const QString &path)
{
    if (path.isEmpty()) return;
    if (processor.isBusy() || exportBusy) return;
    const QFileInfo source(path);
    if (!source.isFile() || (!path.endsWith(QStringLiteral(".pdf"), Qt::CaseInsensitive) &&
                              !QImageReader(path).canRead())) {
        showError(tr("This file is not a readable image or PDF: %1").arg(path));
        return;
    }
    if (!maybeSave()) return;
    processor.resetSource();
    originalPath = QFileInfo(path).absoluteFilePath();
    projectPath.clear();
    document = {};
    document.sourcePath = originalPath;
    undoStack.clear();
    externalDirty = false;
    dirty = false;
    updateTitle();
    wordList->clear();
    displayedWords.clear();
    selectedWords.clear();
    selectedRegion = {};
    regionOutline = nullptr;
    selectionEditor->clear();
    applyButton->setEnabled(false);
    regionOcrButton->setEnabled(false);
    wordBoxes.clear();
    scene->clear();
    imagePath.clear();
    rawImagePath.clear();
    rawImage = {};
    ocrButton->setEnabled(false);
    improveAction->setEnabled(false);
    layoutAction->setEnabled(false);
    createRegionAction->setEnabled(false);
    batchAction->setEnabled(false);
    pdfAction->setEnabled(false);
    pdfaAction->setEnabled(false);
    currentPage = 1;
    pageCount = 1;
    updatePageControls();
    if (originalPath.endsWith(QStringLiteral(".pdf"), Qt::CaseInsensitive)) {
        status->setText(tr("Reading PDF…"));
        processor.openPdf(originalPath);
    } else {
        loadImage(originalPath);
    }
}

void MainWindow::openProject()
{
    if (processor.isBusy() || exportBusy) return;
    const QString path = QFileDialog::getOpenFileName(this, tr("Open YAGF project"), {},
                                                     tr("YAGF projects (*.yagf.json);;JSON (*.json)"));
    if (path.isEmpty() || !maybeSave()) return;
    OcrDocument loaded;
    QString error;
    if (!ProjectFile::load(path, &loaded, &error)) { showError(error); return; }
    if (!QFileInfo::exists(loaded.sourcePath)) {
        showError(tr("Source document is missing: %1").arg(loaded.sourcePath));
        return;
    }
    processor.resetSource();
    undoStack.clear();
    originalPath = loaded.sourcePath;
    projectPath = path;
    document = std::move(loaded);
    externalDirty = false;
    languageEdit->setText(document.language);
    dirty = false;
    updateTitle();
    wordList->clear();
    displayedWords.clear();
    selectedWords.clear();
    selectedRegion = {};
    regionOutline = nullptr;
    selectionEditor->clear();
    applyButton->setEnabled(false);
    regionOcrButton->setEnabled(false);
    wordBoxes.clear();
    scene->clear();
    imagePath.clear();
    rawImagePath.clear();
    rawImage = {};
    improveAction->setEnabled(false);
    layoutAction->setEnabled(false);
    createRegionAction->setEnabled(false);
    batchAction->setEnabled(false);
    pdfAction->setEnabled(false);
    pdfaAction->setEnabled(false);
    currentPage = 1;
    pageCount = 1;
    updatePageControls();
    if (originalPath.endsWith(QStringLiteral(".pdf"), Qt::CaseInsensitive)) {
        status->setText(tr("Reading project source PDF…"));
        processor.openPdf(originalPath);
    } else {
        loadImage(originalPath);
    }
}

bool MainWindow::saveProject()
{
    if (document.sourcePath.isEmpty() || document.pages.isEmpty()) {
        showError(tr("Recognize a document before saving a project."));
        return false;
    }
    if (projectPath.isEmpty()) return saveProjectAs();
    QString error;
    if (!ProjectFile::save(projectPath, document, &error)) { showError(error); return false; }
    externalDirty = false;
    undoStack.setClean();
    dirty = false;
    updateTitle();
    statusBar()->showMessage(tr("Project saved: %1").arg(projectPath));
    return true;
}

bool MainWindow::saveProjectAs()
{
    if (document.sourcePath.isEmpty() || document.pages.isEmpty()) {
        showError(tr("Recognize a document before saving a project."));
        return false;
    }
    QString path = QFileDialog::getSaveFileName(this, tr("Save YAGF project"), projectPath,
                                                tr("YAGF projects (*.yagf.json)"));
    if (path.isEmpty()) return false;
    if (!path.endsWith(QStringLiteral(".yagf.json"), Qt::CaseInsensitive))
        path += QStringLiteral(".yagf.json");
    QString error;
    if (!ProjectFile::save(path, document, &error)) { showError(error); return false; }
    projectPath = path;
    externalDirty = false;
    undoStack.setClean();
    dirty = false;
    updateTitle();
    statusBar()->showMessage(tr("Project saved: %1").arg(projectPath));
    return true;
}

bool MainWindow::maybeSave()
{
    if (!maybeApplyEditor()) return false;
    if (!dirty) return true;
    const auto choice = QMessageBox::warning(this, tr("Unsaved changes"),
        tr("Save the current OCR project before closing it?"),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (choice == QMessageBox::Cancel) return false;
    if (choice == QMessageBox::Save) return saveProject();
    return true;
}

void MainWindow::markDirty()
{
    externalDirty = true;
    refreshDirty();
}

void MainWindow::refreshDirty()
{
    dirty = externalDirty || !undoStack.isClean();
    updateTitle();
}

void MainWindow::updateTitle()
{
    const QString name = projectPath.isEmpty() ? tr("Untitled") : QFileInfo(projectPath).fileName();
    setWindowTitle(tr("YAGF Revival — %1%2").arg(name, dirty ? QStringLiteral(" *") : QString()));
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (maybeSave()) event->accept();
    else event->ignore();
}

void MainWindow::loadImage(const QString &path)
{
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const bool oriented = reader.transformation() != QImageIOHandler::TransformationNone;
    const QImage source = reader.read();
    if (source.isNull()) {
        showError(tr("Could not read image: %1").arg(reader.errorString()));
        return;
    }
    const OcrPage *page = document.page(currentPage);
    if (page && (page->sourceWidth != source.width() || page->sourceHeight != source.height())) {
        showError(tr("The source image size changed since this project was saved."));
        return;
    }
    const core::PreprocessSettings settings = page ? page->preprocessing : core::PreprocessSettings{};
    const auto processed = ImageProcessor::apply(source, settings);
    if (!processed) { showError(processed.error()); return; }
    const QImage &image = *processed;
    if (page && (page->width != image.width() || page->height != image.height())) {
        showError(tr("The processed page size does not match the saved OCR geometry."));
        return;
    }
    QString recognitionPath = path;
    if (!settings.isIdentity() || oriented) {
        recognitionPath = preparedDir.filePath(QStringLiteral("page-%1-prepared.png").arg(currentPage));
        if (!image.save(recognitionPath, "PNG")) {
            showError(tr("Could not save the prepared page image."));
            return;
        }
    }
    rawImage = source;
    displayedPreprocessing = settings;
    rawImagePath = path;
    imagePath = recognitionPath;
    wordBoxes.clear();
    displayedWords.clear();
    selectedWords.clear();
    selectedRegion = {};
    regionOutline = nullptr;
    selectionEditor->clear();
    applyButton->setEnabled(false);
    regionOcrButton->setEnabled(false);
    scene->clear();
    scene->addPixmap(QPixmap::fromImage(image));
    if (currentPage >= 1 && currentPage <= pageList->count())
        pageList->item(currentPage - 1)->setIcon(QIcon(QPixmap::fromImage(
            image.scaled(pageList->iconSize(), Qt::KeepAspectRatio, Qt::SmoothTransformation))));
    scene->setSceneRect(QRectF(QPointF(0, 0), image.size()));
    view->fitPage();
    showDocument();
    setBusy(false);
    status->setText(tr("Page ready: %1 × %2%3").arg(image.width()).arg(image.height())
                    .arg(settings.isIdentity() ? QString() : tr(" · improved")));
}

void MainWindow::showPage(int number)
{
    if (number < 1 || number > pageCount || processor.isBusy() || exportBusy) return;
    if (!maybeApplyEditor()) return;
    currentPage = number;
    updatePageControls();
    if (const OcrPage *page = document.page(number))
        languageEdit->setText(page->language);
    wordList->clear();
    displayedWords.clear();
    selectedWords.clear();
    selectedRegion = {};
    regionOutline = nullptr;
    selectionEditor->clear();
    applyButton->setEnabled(false);
    regionOcrButton->setEnabled(false);
    wordBoxes.clear();
    scene->clear();
    imagePath.clear();
    rawImagePath.clear();
    rawImage = {};
    ocrButton->setEnabled(false);
    improveAction->setEnabled(false);
    layoutAction->setEnabled(false);
    createRegionAction->setEnabled(false);
    if (originalPath.endsWith(QStringLiteral(".pdf"), Qt::CaseInsensitive)) {
        status->setText(tr("Rendering page %1 of %2…").arg(number).arg(pageCount));
        processor.renderPdfPage(number);
    } else {
        loadImage(originalPath);
    }
}

void MainWindow::improveCurrentPage()
{
    if (rawImage.isNull() || processor.isBusy() || !maybeApplyEditor()) return;
    const OcrPage *existing = document.page(currentPage);
    const core::PreprocessSettings current = existing ? existing->preprocessing : core::PreprocessSettings{};
    PreprocessDialog dialog(rawImage, current, this);
    if (dialog.exec() != QDialog::Accepted) return;
    const core::PreprocessSettings proposed = dialog.settings();
    if (proposed == current) return;
    const bool geometryChanged = std::abs(proposed.deskewDegrees - current.deskewDegrees) >= 0.01;
    if (geometryChanged && existing && !existing->words.isEmpty() &&
        QMessageBox::warning(this, tr("Rebuild page geometry"),
            tr("Changing the deskew angle moves the scan and will clear OCR boxes and corrections on this page. Continue?"),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) return;
    const auto processed = ImageProcessor::apply(rawImage, proposed);
    if (!processed) { showError(processed.error()); return; }
    if (!proposed.isIdentity() &&
        !processed->save(preparedDir.filePath(QStringLiteral("page-%1-prepared.png").arg(currentPage)), "PNG")) {
        showError(tr("Could not save the prepared page image."));
        return;
    }
    OcrPage page = existing ? *existing : OcrPage{};
    page.id = QStringLiteral("page-%1").arg(currentPage);
    page.number = currentPage;
    page.sourceWidth = rawImage.width();
    page.sourceHeight = rawImage.height();
    page.width = processed->width();
    page.height = processed->height();
    page.preprocessing = proposed;
    if (geometryChanged) {
        page.words.clear();
        page.regions.clear();
    }
    undoStack.push(new ReplacePageCommand(document, currentPage, std::move(page), tr("Improve page")));
    status->setText(tr("Improvement applied to page %1. Recognize it to update OCR.").arg(currentPage));
}

void MainWindow::runOcr()
{
    if (imagePath.isEmpty() || processor.isBusy()) return;
    if (!maybeApplyEditor()) return;
    status->setText(tr("Recognizing…"));
    processor.recognize(imagePath, languageEdit->text().trimmed(), currentPage);
}

void MainWindow::runBatchOcr()
{
    if (processor.isBusy() || pageCount < 2 ||
        !originalPath.endsWith(QStringLiteral(".pdf"), Qt::CaseInsensitive) || !maybeApplyEditor()) return;
    bool hasCorrections = false;
    for (const OcrPage &page : document.pages)
        for (const OcrWord &word : page.words) hasCorrections |= word.reviewed;
    if (hasCorrections && QMessageBox::question(this, tr("Recognize all pages again"),
        tr("Matching corrections will be kept, but unmatched words may be replaced. Continue?"))
        != QMessageBox::Yes) return;
    QVector<BatchPageOptions> options;
    options.reserve(pageCount);
    for (int number = 1; number <= pageCount; ++number) {
        const OcrPage *page = document.page(number);
        options.append({.number = number,
                        .language = page && !page->language.isEmpty() ? page->language : languageEdit->text().trimmed(),
                        .preprocessing = page ? page->preprocessing : core::PreprocessSettings{}});
    }
    processor.recognizePages(options);
}

void MainWindow::editLayout()
{
    if (imagePath.isEmpty() || processor.isBusy() || !maybeApplyEditor()) return;
    OcrPage *page = document.page(currentPage);
    if (!page || page->regions.isEmpty()) return;
    LayoutDialog dialog(*page, QImage(imagePath), this);
    if (dialog.exec() != QDialog::Accepted || !dialog.hasChanges()) return;
    const OcrPage target = dialog.editedPage();
    bool changed = false;
    for (int i = 0; i < target.regions.size(); ++i) {
        const OcrRegion &wanted = target.regions.at(i);
        const OcrRegion &current = page->regions.at(i);
        changed |= wanted.id != current.id || wanted.type != current.type || wanted.bounds != current.bounds;
    }
    if (!changed) return;
    undoStack.beginMacro(tr("Edit layout"));
    for (int desired = 0; desired < target.regions.size(); ++desired) {
        const QString id = target.regions.at(desired).id;
        int actual = -1;
        for (int j = 0; j < document.page(currentPage)->regions.size(); ++j)
            if (document.page(currentPage)->regions.at(j).id == id) { actual = j; break; }
        while (actual > desired) {
            undoStack.push(new ChangeReadingOrderCommand(document, currentPage, actual, -1));
            --actual;
        }
    }
    for (const OcrRegion &wanted : target.regions) {
        const OcrRegion *current = nullptr;
        for (const OcrRegion &region : document.page(currentPage)->regions)
            if (region.id == wanted.id) { current = &region; break; }
        if (!current) continue;
        if (current->type != wanted.type)
            undoStack.push(new ChangeRegionTypeCommand(document, currentPage, wanted.id, wanted.type));
        current = nullptr;
        for (const OcrRegion &region : document.page(currentPage)->regions)
            if (region.id == wanted.id) { current = &region; break; }
        if (current && current->bounds != wanted.bounds)
            undoStack.push(new MoveRegionCommand(document, currentPage, wanted.id, wanted.bounds));
    }
    undoStack.endMacro();
    status->setText(tr("Page %1 layout and reading order updated").arg(currentPage));
}

void MainWindow::createRegionFromSelection()
{
    if (selectedWords.isEmpty() || processor.isBusy() || !maybeApplyEditor()) return;
    OcrPage *page = document.page(currentPage);
    if (!page) return;
    OcrPage edited = *page;
    if (!edited.extractRegion(selectedWords)) return;
    undoStack.push(new ExtractRegionCommand(document, currentPage, std::move(edited)));
    status->setText(tr("Created a region from selected words. Use Edit layout to set its type and order."));
}

void MainWindow::showDocument()
{
    const QSignalBlocker blockListSignals(wordList);
    wordList->clear();
    for (QGraphicsRectItem *box : wordBoxes) {
        scene->removeItem(box);
        delete box;
    }
    wordBoxes.clear();
    displayedWords.clear();
    OcrPage *page = document.page(currentPage);
    if (!page) {
        updateSelection({});
        layoutAction->setEnabled(false);
        pdfAction->setEnabled(false);
        pdfaAction->setEnabled(false);
        return;
    }
    for (OcrWord &word : page->words) {
        const int i = displayedWords.size();
        displayedWords.append(&word);
        auto *box = scene->addRect(word.bounds, QPen(QColor(20, 90, 220), 1.5));
        box->setBrush(QColor(20, 90, 220, 35));
        box->setData(0, i);
        box->setZValue(1);
        wordBoxes.append(box);
        auto *item = new QListWidgetItem(word.text, wordList);
        if (word.reviewed) item->setForeground(QColor(0, 90, 20));
        item->setToolTip(tr("Confidence: %1; box: %2, %3, %4 × %5")
                         .arg(word.confidence).arg(word.bounds.x()).arg(word.bounds.y())
                         .arg(word.bounds.width()).arg(word.bounds.height()));
    }
    QVector<int> all;
    for (int i = 0; i < displayedWords.size(); ++i) all.append(i);
    updateSelection(all);
    pdfAction->setEnabled(!processor.isBusy() && !exportBusy && document.wordCount() > 0);
    pdfaAction->setEnabled(!processor.isBusy() && !exportBusy && document.wordCount() > 0);
}

void MainWindow::selectWord(int index)
{
    if (index < 0 || index >= wordBoxes.size()) return;
    const QString id = displayedWords.at(index)->id;
    if (!maybeApplyEditor()) return;
    const QVector<int> matches = indicesForIds({id});
    if (matches.isEmpty()) return;
    index = matches.first();
    if (regionOutline) {
        scene->removeItem(regionOutline);
        delete regionOutline;
        regionOutline = nullptr;
    }
    selectedRegion = {};
    regionOcrButton->setEnabled(false);
    updateSelection({index});
    view->centerOn(wordBoxes.at(index));
}

void MainWindow::selectRegion(const QRectF &bounds)
{
    if (imagePath.isEmpty()) return;
    if (!maybeApplyEditor()) return;
    if (regionOutline) {
        scene->removeItem(regionOutline);
        delete regionOutline;
    }
    selectedRegion = bounds.toAlignedRect().intersected(scene->sceneRect().toAlignedRect());
    if (selectedRegion.isEmpty()) return;
    regionOutline = scene->addRect(selectedRegion, QPen(QColor(25, 135, 55), 2, Qt::DashLine));
    regionOutline->setZValue(3);
    QVector<int> matches;
    for (int i = 0; i < displayedWords.size(); ++i)
        if (overlapsWord(displayedWords.at(i)->bounds, selectedRegion)) matches.append(i);
    updateSelection(matches);
    regionOcrButton->setEnabled(!processor.isBusy());
    status->setText(tr("Selected region: %1 × %2 px, %3 words")
                    .arg(selectedRegion.width()).arg(selectedRegion.height()).arg(matches.size()));
}

void MainWindow::updateSelection(const QVector<int> &indices)
{
    selectedWords = indices;
    if (createRegionAction) createRegionAction->setEnabled(!processor.isBusy() && !exportBusy && !indices.isEmpty() &&
        document.page(currentPage) && indices.size() < document.page(currentPage)->words.size());
    QSet<int> selectedSet;
    for (int index : indices) selectedSet.insert(index);
    const QSignalBlocker listBlocker(wordList);
    wordList->clearSelection();
    for (int i = 0; i < wordBoxes.size(); ++i) {
        const bool selected = selectedSet.contains(i);
        wordBoxes[i]->setPen(QPen(selected ? QColor(215, 65, 25) : QColor(20, 90, 220), selected ? 2.5 : 1.5));
        wordBoxes[i]->setBrush(selected ? QColor(235, 95, 30, 50) : QColor(20, 90, 220, 35));
        if (selected) wordList->item(i)->setSelected(true);
    }
    const QSignalBlocker editorBlocker(selectionEditor);
    selectionEditor->clear();
    editorPending = false;
    applyButton->setEnabled(!processor.isBusy() && !exportBusy && !indices.isEmpty());
    selectionLabel->setText(indices.isEmpty() ? tr("Recognized text") :
                            tr("Selected text (%1 words)").arg(indices.size()));
    QTextCursor cursor(selectionEditor->document());
    cursor.movePosition(QTextCursor::Start);
    const OcrWord *previous = nullptr;
    for (int index : indices) {
        if (index < 0 || index >= displayedWords.size()) continue;
        const OcrWord *word = displayedWords.at(index);
        if (previous) {
            const QString separator = previous->block != word->block || previous->paragraph != word->paragraph
                ? QStringLiteral("\n\n") : previous->line != word->line ? QStringLiteral("\n") : QStringLiteral(" ");
            cursor.insertText(separator);
        }
        QTextCharFormat format;
        format.setFontWeight(word->bold ? QFont::Bold : QFont::Normal);
        format.setFontItalic(word->italic);
        if (word->fontSizePt > 0) format.setFontPointSize(word->fontSizePt);
        cursor.insertText(word->text, format);
        previous = word;
    }
    selectionEditor->moveCursor(QTextCursor::Start);
}

QVector<int> MainWindow::indicesForIds(const QStringList &ids) const
{
    QVector<int> result;
    for (int i = 0; i < displayedWords.size(); ++i)
        if (ids.contains(displayedWords.at(i)->id)) result.append(i);
    return result;
}

bool MainWindow::applyCorrection()
{
    if (selectedWords.isEmpty()) { editorPending = false; return true; }
    const QString plain = selectionEditor->toPlainText();
    QRegularExpression wordPattern(QStringLiteral("\\S+"));
    auto matches = wordPattern.globalMatch(plain);
    QVector<core::EditedWord> tokens;
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        QTextCursor cursor(selectionEditor->document());
        cursor.setPosition(match.capturedStart());
        cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
        const QTextCharFormat format = cursor.charFormat();
        tokens.append({.text = match.captured(),
                       .bold = format.fontWeight() >= QFont::Bold,
                       .italic = format.fontItalic(),
                       .fontSizePt = format.fontPointSize() > 0 ? qRound(format.fontPointSize()) : 0});
    }
    const OcrPage *page = document.page(currentPage);
    if (!page) return false;
    auto edited = core::WordEditor::apply(*page, selectedWords, tokens);
    if (!edited) { showError(edited.error()); return false; }
    const int originalCount = selectedWords.size();
    QStringList selectedIds;
    for (int index : selectedWords) selectedIds.append(displayedWords.at(index)->id);
    editorPending = false;
    if (originalCount == 1 && tokens.size() > 1)
        undoStack.push(new SplitWordCommand(document, currentPage, std::move(*edited)));
    else if (originalCount > 1 && tokens.size() == 1)
        undoStack.push(new MergeWordsCommand(document, currentPage, std::move(*edited)));
    else undoStack.push(new ReplaceTextCommand(document, currentPage, std::move(*edited)));
    updateSelection(indicesForIds(selectedIds));
    statusBar()->showMessage(tr("Correction applied. Use Edit → Undo to reverse it."));
    return true;
}

bool MainWindow::maybeApplyEditor()
{
    if (!editorPending) return true;
    return applyCorrection();
}

void MainWindow::runRegionOcr()
{
    if (selectedRegion.isEmpty() || imagePath.isEmpty() || processor.isBusy()) return;
    if (!maybeApplyEditor()) return;
    bool containsCorrections = false;
    for (int index : selectedWords)
        containsCorrections = containsCorrections || displayedWords.at(index)->reviewed;
    if (containsCorrections && QMessageBox::question(this, tr("Recognize region again"),
        tr("This region contains corrected words. New OCR may replace unmatched corrections. Continue?"))
        != QMessageBox::Yes) return;
    status->setText(tr("Recognizing selected region…"));
    processor.recognizeRegion(imagePath, languageEdit->text().trimmed(), currentPage, selectedRegion);
}

void MainWindow::exportText()
{
    if (!maybeApplyEditor()) return;
    if (document.wordCount() == 0) return;
    const QString path = QFileDialog::getSaveFileName(this, tr("Export text"), {}, tr("Text (*.txt)"));
    if (path.isEmpty()) return;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) { showError(file.errorString()); return; }
    file.write(document.plainText().toUtf8());
}

void MainWindow::exportHtml()
{
    if (!maybeApplyEditor() || document.wordCount() == 0) return;
    const QString path = QFileDialog::getSaveFileName(this, tr("Export layout HTML"), {}, tr("HTML (*.html)"));
    if (path.isEmpty()) return;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) { showError(file.errorString()); return; }
    file.write(document.layoutHtml().toUtf8());
}

void MainWindow::exportJson()
{
    if (!maybeApplyEditor()) return;
    if (document.wordCount() == 0) return;
    const QString path = QFileDialog::getSaveFileName(this, tr("Export structured OCR"), {}, tr("JSON (*.json)"));
    if (path.isEmpty()) return;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) { showError(file.errorString()); return; }
    file.write(QJsonDocument(document.toJson()).toJson(QJsonDocument::Indented));
}

void MainWindow::exportPdf(bool archive)
{
    if (processor.isBusy() || exportBusy || document.wordCount() == 0 || !maybeApplyEditor()) return;
    if (document.pages.size() < document.totalPages &&
        QMessageBox::question(this, tr("Pages without OCR"),
            archive ? tr("Some pages have not been recognized in YAGF. OCRmyPDF may recognize them during PDF/A conversion without YAGF corrections. Export anyway?")
                    : tr("Some pages have not been recognized and will have no searchable text. Export anyway?"),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) return;
    QString path = QFileDialog::getSaveFileName(this,
        archive ? tr("Export archival PDF/A") : tr("Export searchable PDF"), {}, tr("PDF files (*.pdf)"));
    if (path.isEmpty()) return;
    if (!path.endsWith(QStringLiteral(".pdf"), Qt::CaseInsensitive)) path += QStringLiteral(".pdf");
    pdfExportPath = path;
    pdfCancel = std::make_shared<std::atomic_bool>(false);
    exportBusy = true;
    setBusy(true);
    status->setText(tr("Preparing PDF…"));
    const OcrDocument snapshot = document;
    const auto cancel = pdfCancel;
    pdfWatcher.setFuture(QtConcurrent::run([this, snapshot, path, archive, cancel] {
        return processing::PdfExporter::write(snapshot, path, archive, *cancel,
            [this](int completed, int total) {
                QMetaObject::invokeMethod(this, [this, completed, total] {
                    if (exportBusy) status->setText(tr("PDF export: %1 of %2 pages").arg(completed).arg(total));
                }, Qt::QueuedConnection);
            });
    }));
}

} // namespace yagf::ui

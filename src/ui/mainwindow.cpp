#include "mainwindow.h"
#include "projectfile.h"
#include "preprocessdialog.h"
#include "imageprocessor.h"
#include "layoutdialog.h"
#include "scannerdialog.h"
#include "documentcommands.h"
#include "wordeditor.h"
#include "remoteocrengine.h"
#include "tesseractengine.h"
#include "aireviewdialog.h"

#include <QCloseEvent>
#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QActionGroup>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGraphicsPixmapItem>
#include <QGraphicsPathItem>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QHBoxLayout>
#include <QImageReader>
#include <QImageIOHandler>
#include <QKeyEvent>
#include <QIcon>
#include <QJsonDocument>
#include <QMetaObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
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
#include <QProcess>
#include <QSignalBlocker>
#include <QSaveFile>
#include <QSettings>
#include <QSplitter>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyle>
#include <QToolBar>
#include <QToolButton>
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
using core::ImageRegion;
using core::ProjectFile;
using processing::DocumentProcessor;
using processing::ImageProcessor;
using processing::BatchPageOptions;

namespace {
bool overlapsBox(const QRect &box, const QRect &selection)
{
    if (selection.contains(box.center())) return true;
    const QRect overlap = box.intersected(selection);
    return !overlap.isEmpty() &&
        static_cast<qint64>(overlap.width()) * overlap.height() * 4 >=
        static_cast<qint64>(box.width()) * box.height();
}

bool overlapsWord(const OcrWord &word, const QRect &selection)
{
    if (word.sourceBoxes.isEmpty()) return overlapsBox(word.bounds, selection);
    for (const QRect &fragment : word.sourceBoxes)
        if (overlapsBox(fragment, selection)) return true;
    return false;
}

void carryCorrections(OcrPage &fresh, const OcrPage *oldPage)
{
    if (!oldPage) return;
    fresh.imageRegions = oldPage->imageRegions;
    const bool structured = fresh.engineId == "surya-remote" || oldPage->engineId == "surya-remote";
    const bool sameEngine = oldPage->engineId == fresh.engineId;
    // Keep manual layout on a matching block. IDs alone can be reused for
    // completely different content after switching recognition engines.
    QSet<int> matchingBlocks;
    for (OcrWord &word : fresh.words) {
        for (const OcrWord &old : oldPage->words) {
            if (old.bounds != word.bounds || old.originalText != word.originalText) continue;
            if (structured && sameEngine && word.block == old.block) matchingBlocks.insert(word.block);
            word.id = old.id;
            word.bold = old.bold;
            word.italic = old.italic;
            word.sourceWordIds = old.sourceWordIds;
            word.sourceBoxes = old.sourceBoxes;
            word.fontSizePt = old.fontSizePt;
            word.sourceFontSizePt = old.sourceFontSizePt;
            if (old.reviewed) { word.text = old.text; word.reviewed = true; }
            break;
        }
    }
    if (structured) {
        QHash<QString, core::OcrRegion> oldRegions;
        for (const auto &region : oldPage->regions) oldRegions.insert(region.id, region);
        QSet<QString> matchedIds;
        for (int block : matchingBlocks)
            matchedIds.insert(QStringLiteral("page-%1/region-%2").arg(fresh.number).arg(block));
        for (auto &region : fresh.regions) {
            const auto old = oldRegions.constFind(region.id);
            if (old != oldRegions.cend() && matchedIds.contains(region.id)) {
                region.bounds = old->bounds;
                region.type = old->type;
            }
        }
        if (matchedIds.size() != fresh.regions.size()) return;
    } else {
        fresh.regions = oldPage->regions;
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

PageView::PageView(QWidget *parent) : QGraphicsView(parent), rubberBand(new QRubberBand(QRubberBand::Rectangle, viewport()))
{
    setMouseTracking(true);
    viewport()->setMouseTracking(true);
}

void PageView::setRegionMode(bool enabled)
{
    regionMode = enabled;
    rubberBand->hide();
    editingRegionIndex = -1;
    editingRegionItem = nullptr;
    setDragMode(enabled ? QGraphicsView::NoDrag : QGraphicsView::ScrollHandDrag);
    viewport()->setCursor(enabled ? Qt::CrossCursor : Qt::ArrowCursor);
}

void PageView::setActiveRegionIndex(int index)
{
    activeRegionIndex = index;
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
    if (regionMode && event->button() == Qt::LeftButton &&
        event->modifiers().testFlag(Qt::ControlModifier)) {
        for (QGraphicsItem *hit : items(event->pos()))
            if (auto *box = qgraphicsitem_cast<QGraphicsPathItem *>(hit);
                box && box->data(0).isValid()) {
                emit wordClicked(box->data(0).toInt());
                event->accept();
                return;
            }
    }
    if (regionMode && event->button() == Qt::LeftButton) {
        setFocus();
        if (auto *item = qgraphicsitem_cast<QGraphicsRectItem *>(itemAt(event->pos()));
            item && item->data(1).isValid()) {
            const int index = item->data(1).toInt();
            emit regionActivated(index);
            item = qgraphicsitem_cast<QGraphicsRectItem *>(itemAt(event->pos()));
            if (!item || !item->data(1).isValid() || item->data(1).toInt() != index) {
                event->accept();
                return;
            }
            editingRegionIndex = index;
            activeRegionIndex = editingRegionIndex;
            editingRegionItem = item;
            editStartRect = item->rect();
            editStartScene = mapToScene(event->pos());
            const qreal tolerance = 8.0 / qMax<qreal>(0.05, transform().m11());
            const QPointF point = editStartScene;
            resizeEdges = 0;
            if (std::abs(point.x() - editStartRect.left()) <= tolerance) resizeEdges |= 1;
            else if (std::abs(point.x() - editStartRect.right()) <= tolerance) resizeEdges |= 2;
            if (std::abs(point.y() - editStartRect.top()) <= tolerance) resizeEdges |= 4;
            else if (std::abs(point.y() - editStartRect.bottom()) <= tolerance) resizeEdges |= 8;
            event->accept();
            return;
        }
        regionOrigin = event->pos();
        rubberBand->setGeometry(QRect(regionOrigin, QSize()));
        rubberBand->show();
        event->accept();
        return;
    }
    for (QGraphicsItem *hit : items(event->pos()))
        if (auto *box = qgraphicsitem_cast<QGraphicsPathItem *>(hit);
            box && box->data(0).isValid()) {
            emit wordClicked(box->data(0).toInt());
            break;
        }
    QGraphicsView::mousePressEvent(event);
}

void PageView::mouseMoveEvent(QMouseEvent *event)
{
    if (regionMode && editingRegionItem) {
        const QPointF delta = mapToScene(event->pos()) - editStartScene;
        QRectF next = editStartRect;
        const QRectF limits = scene()->sceneRect();
        if (resizeEdges == 0) {
            next.moveLeft(qBound(limits.left(), editStartRect.left() + delta.x(),
                                 limits.right() - editStartRect.width()));
            next.moveTop(qBound(limits.top(), editStartRect.top() + delta.y(),
                                limits.bottom() - editStartRect.height()));
        } else {
            if (resizeEdges & 1) next.setLeft(qBound(limits.left(), editStartRect.left() + delta.x(),
                                                  editStartRect.right() - 8.0));
            if (resizeEdges & 2) next.setRight(qBound(editStartRect.left() + 8.0,
                                                     editStartRect.right() + delta.x(), limits.right()));
            if (resizeEdges & 4) next.setTop(qBound(limits.top(), editStartRect.top() + delta.y(),
                                                 editStartRect.bottom() - 8.0));
            if (resizeEdges & 8) next.setBottom(qBound(editStartRect.top() + 8.0,
                                                      editStartRect.bottom() + delta.y(), limits.bottom()));
        }
        editingRegionItem->setRect(next);
        event->accept();
        return;
    }
    if (regionMode && rubberBand->isVisible()) {
        rubberBand->setGeometry(QRect(regionOrigin, event->pos()).normalized());
        event->accept();
        return;
    }
    if (regionMode && event->buttons() == Qt::NoButton) {
        Qt::CursorShape cursor = Qt::CrossCursor;
        if (auto *item = qgraphicsitem_cast<QGraphicsRectItem *>(itemAt(event->pos()));
            item && item->data(1).isValid()) {
            const QRectF box = item->rect();
            const QPointF point = mapToScene(event->pos());
            const qreal tolerance = 8.0 / qMax<qreal>(0.05, transform().m11());
            const bool horizontal = std::abs(point.x() - box.left()) <= tolerance ||
                                    std::abs(point.x() - box.right()) <= tolerance;
            const bool vertical = std::abs(point.y() - box.top()) <= tolerance ||
                                  std::abs(point.y() - box.bottom()) <= tolerance;
            cursor = horizontal && vertical ? Qt::SizeFDiagCursor : horizontal ? Qt::SizeHorCursor
                : vertical ? Qt::SizeVerCursor : Qt::SizeAllCursor;
        }
        viewport()->setCursor(cursor);
    }
    QGraphicsView::mouseMoveEvent(event);
}

void PageView::mouseReleaseEvent(QMouseEvent *event)
{
    if (regionMode && event->button() == Qt::LeftButton && editingRegionItem) {
        const int index = editingRegionIndex;
        const QRectF bounds = editingRegionItem->rect();
        const bool changed = bounds.toAlignedRect() != editStartRect.toAlignedRect();
        editingRegionItem = nullptr;
        editingRegionIndex = -1;
        if (changed) emit regionChanged(index, bounds);
        event->accept();
        return;
    }
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

void PageView::keyPressEvent(QKeyEvent *event)
{
    if (regionMode && activeRegionIndex >= 0 &&
        (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace)) {
        emit regionDeleteRequested(activeRegionIndex);
        event->accept();
        return;
    }
    QGraphicsView::keyPressEvent(event);
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
    resize(1280, 820);
    setObjectName(QStringLiteral("yagfWindow"));
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
    toolbar->setObjectName(QStringLiteral("mainToolbar"));
    toolbar->setMovable(false);
    toolbar->setIconSize(QSize(18, 18));
    toolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    fileMenu->addAction(tr("Open image or PDF…"), this, &MainWindow::openFile);
    auto *openAction = toolbar->addAction(style()->standardIcon(QStyle::SP_DialogOpenButton),
                                          tr("Open"), this, &MainWindow::openFile);
    openAction->setToolTip(tr("Open an image or PDF"));
    fileMenu->addAction(tr("Scan a page…"), this, &MainWindow::scanPage);
    auto *pasteAction = fileMenu->addAction(tr("Paste image from clipboard"), this, &MainWindow::pasteImage);
    pasteAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+V")));
    fileMenu->addAction(tr("Open project"), this, &MainWindow::openProject);
    fileMenu->addAction(tr("Open portable project folder…"), this, &MainWindow::openPortableProject);
    fileMenu->addAction(tr("Save project"), this, &MainWindow::saveProject);
    auto *saveAction = toolbar->addAction(style()->standardIcon(QStyle::SP_DialogSaveButton),
                                          tr("Save"), this, &MainWindow::saveProject);
    saveAction->setToolTip(tr("Save the current project"));
    fileMenu->addAction(tr("Save project as…"), this, &MainWindow::saveProjectAs);
    fileMenu->addAction(tr("Save portable project folder…"), this, &MainWindow::savePortableProject);
    toolbar->addSeparator();
    previousButton = new QPushButton(QIcon(QStringLiteral(":/legacy/back.png")), {}, this);
    previousButton->setObjectName(QStringLiteral("pageNavButton"));
    previousButton->setToolTip(tr("Previous page"));
    nextButton = new QPushButton(QIcon(QStringLiteral(":/legacy/forward.png")), {}, this);
    nextButton->setObjectName(QStringLiteral("pageNavButton"));
    nextButton->setToolTip(tr("Next page"));
    previousButton->setIconSize(QSize(20, 20));
    nextButton->setIconSize(QSize(20, 20));
    pageSpin = new QSpinBox(this);
    pageSpin->setRange(1, 1);
    pageSpin->setToolTip(tr("PDF page number"));
    pageSpin->setObjectName(QStringLiteral("pageNumber"));
    pageSpin->setMaximumWidth(58);
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
    regionAction->setChecked(true);
    regionAction->setToolTip(tr("Drag on the image to add OCR regions; drag an area to move it or its edges to resize it."));
    connect(regionAction, &QAction::toggled, this, [this](bool enabled) { view->setRegionMode(enabled); });
    auto *zoomOut = toolbar->addAction(QIcon(QStringLiteral(":/legacy/smaller.png")),
                                      tr("Zoom out"), this, [this] { view->zoomBy(1.0 / 1.25); });
    zoomOut->setToolTip(tr("Zoom out"));
    qobject_cast<QToolButton *>(toolbar->widgetForAction(zoomOut))->setToolButtonStyle(Qt::ToolButtonIconOnly);
    toolbar->addAction(tr("Fit page"), this, [this] { view->fitPage(); });
    auto *zoomIn = toolbar->addAction(QIcon(QStringLiteral(":/legacy/larger.png")),
                                     tr("Zoom in"), this, [this] { view->zoomBy(1.25); });
    zoomIn->setToolTip(tr("Zoom in"));
    qobject_cast<QToolButton *>(toolbar->widgetForAction(zoomIn))->setToolButtonStyle(Qt::ToolButtonIconOnly);
    toolbar->addSeparator();
    improveAction = toolbar->addAction(tr("Improve page…"), this, &MainWindow::improveCurrentPage);
    improveAction->setObjectName(QStringLiteral("improvePageAction"));
    improveAction->setEnabled(false);
    toolsMenu->addAction(improveAction);
    layoutAction = toolsMenu->addAction(tr("Edit layout…"), this, &MainWindow::editLayout);
    createRegionAction = toolsMenu->addAction(tr("Create region from selection"), this,
                                             &MainWindow::createRegionFromSelection);
    remainingAction = toolsMenu->addAction(tr("Recognize remaining pages"), this, [this] { runBatchOcr(true); });
    batchAction = toolsMenu->addAction(tr("Recognize all pages"), this, [this] { runBatchOcr(false); });
    similarSymbolsAction = toolsMenu->addAction(tr("Match similar symbol images"));
    similarSymbolsAction->setCheckable(true);
    const QString similarSymbolsKey = QStringLiteral("review/matchSimilarSymbolImages");
    QSettings settings(QStringLiteral("YAGF"), QStringLiteral("YAGF Revival"));
    similarSymbolsAction->setChecked(settings.value(similarSymbolsKey, false).toBool());
    connect(similarSymbolsAction, &QAction::toggled, this, [this, similarSymbolsKey](bool enabled) {
        QSettings(QStringLiteral("YAGF"), QStringLiteral("YAGF Revival")).setValue(similarSymbolsKey, enabled);
        correctionMemory.reset();
        refreshReviewLabel();
    });
    toolsMenu->addAction(tr("OCR engine…"), this, &MainWindow::configureOcrEngine);
    languagePickerAction = toolsMenu->addAction(tr("Choose OCR languages…"), this,
                                                &MainWindow::chooseOcrLanguages);
    layoutAction->setEnabled(false);
    createRegionAction->setEnabled(false);
    batchAction->setEnabled(false);
    ocrButton = new QPushButton(tr("Recognize"), this);
    ocrButton->setObjectName(QStringLiteral("primaryButton"));
    toolbar->addWidget(ocrButton);
    connect(ocrButton, &QPushButton::clicked, this, &MainWindow::runOcr);
    cancelButton = new QPushButton(tr("Cancel"), this);
    cancelButton->setObjectName(QStringLiteral("secondaryButton"));
    cancelButton->setEnabled(false);
    toolbar->addWidget(cancelButton);
    connect(cancelButton, &QPushButton::clicked, &processor, &DocumentProcessor::cancel);
    connect(cancelButton, &QPushButton::clicked, this, [this] {
        if (exportBusy && exportCancel) {
            exportCancel->store(true);
            cancelButton->setEnabled(false);
            status->setText(tr("Cancelling export…"));
        }
    });
    toolbar->addSeparator();
    auto *languageLabel = new QLabel(tr("OCR"), this);
    languageLabel->setObjectName(QStringLiteral("toolbarCaption"));
    toolbar->addWidget(languageLabel);
    languageEdit = new QLineEdit(QStringLiteral("eng"), this);
    languageEdit->setToolTip(tr("Tesseract languages, for example eng+deu. The first language is primary."));
    languageEdit->setObjectName(QStringLiteral("languageField"));
    languageEdit->setMinimumWidth(115);
    languageEdit->setMaximumWidth(135);
    toolbar->addWidget(languageEdit);
    languagePickerButton = new QToolButton(this);
    languagePickerButton->setObjectName(QStringLiteral("languagePickerButton"));
    languagePickerButton->setText(QStringLiteral("…"));
    languagePickerButton->setToolTip(tr("Choose installed Tesseract languages"));
    toolbar->addWidget(languagePickerButton);
    connect(languagePickerButton, &QToolButton::clicked, this, &MainWindow::chooseOcrLanguages);
    fileMenu->addSeparator();
    fileMenu->addAction(tr("Export text"), this, &MainWindow::exportText);
    fileMenu->addAction(tr("Export scientific book as HTML…"), this, &MainWindow::exportScientificBook);
    fileMenu->addAction(tr("Export layout HTML"), this, &MainWindow::exportHtml);
    fileMenu->addAction(tr("Export verified corrections…"), this, &MainWindow::exportVerifiedSamples);
    fileMenu->addAction(tr("Export JSON"), this, &MainWindow::exportJson);
    fileMenu->addSeparator();
    pdfAction = fileMenu->addAction(tr("Export searchable PDF…"), this, [this] { exportPdf(false); });
    pdfaAction = fileMenu->addAction(tr("Export archival PDF/A…"), this, [this] { exportPdf(true); });
    pdfAction->setEnabled(false);
    pdfaAction->setEnabled(false);

    auto *splitter = new QSplitter(this);
    splitter->setObjectName(QStringLiteral("workspaceSplitter"));
    splitter->setHandleWidth(1);
    auto *navigationPane = new QWidget(splitter);
    navigationPane->setObjectName(QStringLiteral("navigationPane"));
    auto *navigationLayout = new QVBoxLayout(navigationPane);
    navigationLayout->setContentsMargins(14, 16, 14, 14);
    navigationLayout->setSpacing(10);
    auto *pagesHeading = new QLabel(tr("PAGES"), navigationPane);
    pagesHeading->setObjectName(QStringLiteral("sectionHeading"));
    navigationLayout->addWidget(pagesHeading);
    pageList = new QListWidget(navigationPane);
    pageList->setObjectName(QStringLiteral("pageMiniatures"));
    pageList->setViewMode(QListView::IconMode);
    pageList->setFlow(QListView::TopToBottom);
    pageList->setWrapping(false);
    pageList->setMovement(QListView::Static);
    pageList->setResizeMode(QListView::Adjust);
    pageList->setGridSize(QSize(170, 190));
    pageList->setIconSize(QSize(126, 156));
    pageList->setMinimumHeight(220);
    pageList->setMaximumHeight(330);
    navigationPane->setMinimumWidth(195);
    navigationPane->setMaximumWidth(245);
    navigationLayout->addWidget(pageList);
    auto *regionsHeading = new QLabel(tr("IMAGE REGIONS"), navigationPane);
    regionsHeading->setObjectName(QStringLiteral("sectionHeading"));
    navigationLayout->addWidget(regionsHeading);
    regionList = new QListWidget(navigationPane);
    regionList->setObjectName(QStringLiteral("imageRegions"));
    regionList->setToolTip(tr("Select an OCR area. Drag its edge on the image to resize it; press Delete to remove it."));
    navigationLayout->addWidget(regionList, 2);
    deleteRegionButton = new QPushButton(tr("Delete region"), navigationPane);
    deleteRegionButton->setObjectName(QStringLiteral("deleteRegionButton"));
    deleteRegionButton->setEnabled(false);
    navigationLayout->addWidget(deleteRegionButton);
    pageList->setToolTip(tr("Select a page miniature to preview it."));
    view = new PageView(splitter);
    view->setObjectName(QStringLiteral("pagePreview"));
    scene = new QGraphicsScene(this);
    view->setScene(scene);
    view->setAcceptDrops(true);
    view->setRegionMode(true);
    view->setRenderHint(QPainter::Antialiasing);
    view->setBackgroundBrush(QColor(QStringLiteral("#e9eef5")));
    auto *textPanel = new QWidget(splitter);
    textPanel->setObjectName(QStringLiteral("textPanel"));
    auto *textLayout = new QVBoxLayout(textPanel);
    textLayout->setContentsMargins(16, 16, 16, 14);
    textLayout->setSpacing(10);
    selectionLabel = new QLabel(tr("Recognized text"), textPanel);
    selectionLabel->setObjectName(QStringLiteral("editorHeading"));
    textLayout->addWidget(selectionLabel);
    auto *formatToolbar = new QToolBar(textPanel);
    formatToolbar->setObjectName(QStringLiteral("formatToolbar"));
    formatToolbar->setMovable(false);
    formatToolbar->setIconSize(QSize(16, 16));
    textLayout->addWidget(formatToolbar);
    selectionEditor = new QTextEdit(textPanel);
    selectionEditor->setObjectName(QStringLiteral("recognizedTextEditor"));
    selectionEditor->setPlaceholderText(tr("Select words on the page or drag a region, then edit here."));
    selectionEditor->setAcceptRichText(true);
    selectionEditor->setMinimumHeight(180);
    textLayout->addWidget(selectionEditor, 2);
    auto *editButtons = new QHBoxLayout();
    applyButton = new QPushButton(tr("Apply correction"), textPanel);
    applyButton->setObjectName(QStringLiteral("applyButton"));
    applyButton->setEnabled(false);
    regionOcrButton = new QPushButton(tr("Recognize region"), textPanel);
    regionOcrButton->setObjectName(QStringLiteral("recognizeRegionButton"));
    regionOcrButton->setEnabled(false);
    editButtons->addWidget(applyButton);
    editButtons->addWidget(regionOcrButton);
    textLayout->addLayout(editButtons);
    reviewWordButton = new QPushButton(tr("Review word…"), textPanel);
    reviewWordButton->setObjectName(QStringLiteral("reviewWordButton"));
    reviewWordButton->setEnabled(false);
    reviewWordButton->setToolTip(tr("Inspect an enlarged source crop and request contextual reading suggestions"));
    textLayout->addWidget(reviewWordButton);
    auto *wordsHeading = new QLabel(tr("WORDS ON THIS PAGE"), textPanel);
    wordsHeading->setObjectName(QStringLiteral("sectionHeading"));
    textLayout->addWidget(wordsHeading);
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
    splitter->addWidget(navigationPane);
    splitter->addWidget(view);
    splitter->addWidget(textPanel);
    splitter->setStretchFactor(1, 3);
    splitter->setStretchFactor(2, 1);
    splitter->setSizes({220, 690, 370});
    splitter->setChildrenCollapsible(false);
    setCentralWidget(splitter);
    QFile styleFile(QStringLiteral(":/theme/app.qss"));
    if (styleFile.open(QIODevice::ReadOnly)) setStyleSheet(QString::fromUtf8(styleFile.readAll()));
    setAcceptDrops(true);
    connect(view, &PageView::inputDropped, this, &MainWindow::importMimeData);
    updatePageControls();
    connect(pageList, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row >= 0) showPage(row + 1);
    });
    connect(view, &PageView::regionSelected, this, &MainWindow::selectRegion);
    connect(view, &PageView::regionActivated, this, &MainWindow::activateImageRegion);
    connect(view, &PageView::regionChanged, this, &MainWindow::moveImageRegion);
    connect(view, &PageView::regionDeleteRequested, this, &MainWindow::deleteImageRegion);
    connect(regionList, &QListWidget::currentRowChanged, this, &MainWindow::activateImageRegion);
    connect(deleteRegionButton, &QPushButton::clicked, this, [this] {
        deleteImageRegion(selectedImageRegion);
    });
    connect(applyButton, &QPushButton::clicked, this, &MainWindow::applyCorrection);
    connect(regionOcrButton, &QPushButton::clicked, this, &MainWindow::runRegionOcr);
    connect(reviewWordButton, &QPushButton::clicked, this, &MainWindow::reviewSelectedWord);
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
    connect(&processor, &DocumentProcessor::failed, this, [this](const QString &message) {
        queuedImageRegions.clear();
        showError(message);
    });
    connect(&processor, &DocumentProcessor::cancelled, this, [this] {
        queuedImageRegions.clear();
        status->setText(tr("Cancelled"));
    });
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
            for (QRect &fragment : word.sourceBoxes) fragment.translate(offset);
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
                        word.sourceBoxes = old.sourceBoxes;
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
            QHash<QString, QString> regionIds;
            for (OcrWord &word : newPage.words) {
                const QString prefix = QStringLiteral("page-%1/region-").arg(currentPage);
                regionIds.insert(prefix + QString::number(word.block), prefix + QString::number(word.block + maxBlock));
                word.block += maxBlock;
            }
            // Region OCR returns crop-local boxes and block IDs.
            for (auto detected : newPage.regions) {
                detected.id = regionIds.value(detected.id);
                detected.bounds.translate(offset);
                merged.regions.append(std::move(detected));
            }
            QVector<OcrWord> kept;
            int insertion = -1;
            for (const OcrWord &old : merged.words) {
                if (overlapsWord(old, region)) {
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
        if (!region.isEmpty()) {
            const OcrPage *page = document.page(currentPage);
            for (int i = 0; page && i < page->imageRegions.size(); ++i)
                if (page->imageRegions.at(i).bounds == region) {
                    activateImageRegion(i);
                    break;
                }
        }
        if (!queuedImageRegions.isEmpty()) {
            ++nextQueuedImageRegion;
            status->setText(tr("Recognized region %1 of %2")
                            .arg(nextQueuedImageRegion).arg(queuedImageRegions.size()));
            QTimer::singleShot(0, this, &MainWindow::runNextImageRegion);
        } else status->setText(tr("Page %1: %2 words recognized")
                             .arg(currentPage).arg(document.page(currentPage)->words.size()));
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
        status->setText(tr("Batch OCR complete"));
    });
    connect(&exportWatcher, &QFutureWatcher<processing::ExportResult>::finished, this, [this] {
        const auto result = exportWatcher.result();
        exportBusy = false;
        exportCancel.reset();
        setBusy(processor.isBusy());
        if (result.status == processing::ExportResult::Status::Success)
            status->setText(tr("Exported: %1").arg(exportPath));
        else if (result.status == processing::ExportResult::Status::Cancelled)
            status->setText(tr("Export cancelled"));
        else showError(result.error);
    });
    connect(&undoStack, &QUndoStack::indexChanged, this, [this] {
        correctionMemory.reset();
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
    if (exportBusy && exportCancel) {
        exportCancel->store(true);
        exportWatcher.waitForFinished();
    }
}

void MainWindow::setBusy(bool busy)
{
    const bool blocked = busy || exportBusy;
    undoAction->setEnabled(!blocked && undoStack.canUndo());
    redoAction->setEnabled(!blocked && undoStack.canRedo());
    ocrButton->setEnabled(!blocked && !imagePath.isEmpty());
    view->setEnabled(!blocked);
    cancelButton->setEnabled(blocked);
    selectionEditor->setEnabled(!blocked);
    applyButton->setEnabled(!blocked && !selectedWords.isEmpty() && !selectionEditor->isReadOnly());
    regionOcrButton->setEnabled(!blocked && !imagePath.isEmpty() && !selectedRegion.isEmpty());
    reviewWordButton->setEnabled(!blocked && !imagePath.isEmpty() && selectedWords.size() == 1);
    regionList->setEnabled(!blocked);
    deleteRegionButton->setEnabled(!blocked && selectedImageRegion >= 0);
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
    if (remainingAction) remainingAction->setEnabled(batchAction->isEnabled());
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
    correctionMemory.reset();
    document.sourcePath = originalPath;
    undoStack.clear();
    externalDirty = false;
    dirty = false;
    updateTitle();
    wordList->clear();
    displayedWords.clear();
    selectedWords.clear();
    selectedRegion = {};
    selectedImageRegion = -1;
    queuedImageRegions.clear();
    selectionEditor->clear();
    applyButton->setEnabled(false);
    regionOcrButton->setEnabled(false);
    wordBoxes.clear();
    imageRegionBoxes.clear();
    regionList->clear();
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
    loadProject(path);
}

void MainWindow::openPortableProject()
{
    if (processor.isBusy() || exportBusy) return;
    const QString directory = QFileDialog::getExistingDirectory(this, tr("Open portable YAGF project folder"));
    loadProject(directory);
}

void MainWindow::loadProject(const QString &path)
{
    if (processor.isBusy() || exportBusy || path.isEmpty()) return;
    if (!maybeSave()) return;
    OcrDocument loaded;
    QString error;
    const bool portable = QFileInfo(path).isDir() || path.endsWith(QStringLiteral(".yagfproj"), Qt::CaseInsensitive);
    const bool opened = portable ? ProjectFile::loadPortable(path, &loaded, &error)
                                 : ProjectFile::load(path, &loaded, &error);
    if (!opened) { showError(error); return; }
    if (!QFileInfo::exists(loaded.sourcePath)) {
        showError(tr("Source document is missing: %1").arg(loaded.sourcePath));
        return;
    }
    processor.resetSource();
    undoStack.clear();
    originalPath = loaded.sourcePath;
    projectPath = path;
    document = std::move(loaded);
    correctionMemory.reset();
    externalDirty = false;
    if (document.engine == processor.engineId() && !document.language.isEmpty())
        languageEdit->setText(document.language);
    dirty = false;
    updateTitle();
    wordList->clear();
    displayedWords.clear();
    selectedWords.clear();
    selectedRegion = {};
    selectedImageRegion = -1;
    queuedImageRegions.clear();
    selectionEditor->clear();
    applyButton->setEnabled(false);
    regionOcrButton->setEnabled(false);
    wordBoxes.clear();
    imageRegionBoxes.clear();
    regionList->clear();
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
    if (!maybeApplyEditor()) return false;
    if (document.sourcePath.isEmpty() || document.pages.isEmpty()) {
        showError(tr("Recognize a document before saving a project."));
        return false;
    }
    if (projectPath.isEmpty()) return saveProjectAs();
    QString error;
    const bool portable = projectPath.endsWith(QStringLiteral(".yagfproj"), Qt::CaseInsensitive) ||
                          QFileInfo(projectPath).isDir();
    const bool saved = portable ? ProjectFile::savePortable(projectPath, document, &error)
                                : ProjectFile::save(projectPath, document, &error);
    if (!saved) { showError(error); return false; }
    externalDirty = false;
    undoStack.setClean();
    dirty = false;
    updateTitle();
    statusBar()->showMessage(tr("Project saved: %1").arg(projectPath));
    return true;
}

bool MainWindow::saveProjectAs()
{
    if (!maybeApplyEditor()) return false;
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

bool MainWindow::savePortableProject()
{
    if (document.sourcePath.isEmpty() || document.pages.isEmpty() || !maybeApplyEditor()) {
        if (document.pages.isEmpty()) showError(tr("Recognize a document before saving a project."));
        return false;
    }
    QString directory = QFileDialog::getSaveFileName(this, tr("Save portable YAGF project folder"), {},
                                                     tr("Portable project folders (*.yagfproj)"));
    if (directory.isEmpty()) return false;
    if (!directory.endsWith(QStringLiteral(".yagfproj"), Qt::CaseInsensitive))
        directory += QStringLiteral(".yagfproj");
    QString error;
    if (!ProjectFile::savePortable(directory, document, &error)) { showError(error); return false; }
    projectPath = directory;
    externalDirty = false;
    undoStack.setClean();
    dirty = false;
    updateTitle();
    statusBar()->showMessage(tr("Portable project saved: %1").arg(directory));
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
    imageRegionBoxes.clear();
    displayedWords.clear();
    selectedWords.clear();
    selectedRegion = {};
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
    if (const OcrPage *page = document.page(number);
        page && page->engineId == processor.engineId() && !page->language.isEmpty())
        languageEdit->setText(page->language);
    wordList->clear();
    displayedWords.clear();
    selectedWords.clear();
    selectedRegion = {};
    selectedImageRegion = -1;
    queuedImageRegions.clear();
    selectionEditor->clear();
    applyButton->setEnabled(false);
    regionOcrButton->setEnabled(false);
    wordBoxes.clear();
    imageRegionBoxes.clear();
    regionList->clear();
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
    if (geometryChanged && existing && (!existing->words.isEmpty() || !existing->imageRegions.isEmpty()) &&
        QMessageBox::warning(this, tr("Rebuild page geometry"),
            tr("Changing the deskew angle moves the scan and will clear OCR boxes, corrections, and image regions on this page. Continue?"),
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
        page.imageRegions.clear();
        selectedImageRegion = -1;
    }
    undoStack.push(new ReplacePageCommand(document, currentPage, std::move(page), tr("Improve page")));
    status->setText(tr("Improvement applied to page %1. Recognize it to update OCR.").arg(currentPage));
}

void MainWindow::runOcr()
{
    if (imagePath.isEmpty() || processor.isBusy()) return;
    if (!maybeApplyEditor()) return;
    if (const OcrPage *page = document.page(currentPage);
        page && !page->imageRegions.isEmpty()) {
        bool hasCorrections = false;
        for (const OcrWord &word : page->words)
            if (word.reviewed)
                for (const ImageRegion &region : page->imageRegions)
                    hasCorrections |= overlapsWord(word, region.bounds);
        if (hasCorrections && QMessageBox::question(this, tr("Recognize image regions"),
            tr("Some selected regions contain corrected text. New OCR may replace unmatched corrections. Continue?"))
            != QMessageBox::Yes) return;
        queuedImageRegions.clear();
        for (const ImageRegion &region : page->imageRegions) queuedImageRegions.append(region.bounds);
        nextQueuedImageRegion = 0;
        runNextImageRegion();
        return;
    }
    status->setText(tr("Recognizing…"));
    processor.recognize(imagePath, languageEdit->text().trimmed(), currentPage);
}

void MainWindow::runNextImageRegion()
{
    if (queuedImageRegions.isEmpty()) return;
    if (nextQueuedImageRegion >= queuedImageRegions.size()) {
        const int count = queuedImageRegions.size();
        queuedImageRegions.clear();
        status->setText(tr("Recognized %1 image regions").arg(count));
        return;
    }
    if (processor.isBusy() || imagePath.isEmpty()) return;
    status->setText(tr("Recognizing region %1 of %2…")
                    .arg(nextQueuedImageRegion + 1).arg(queuedImageRegions.size()));
    processor.recognizeRegion(imagePath, languageEdit->text().trimmed(), currentPage,
                              queuedImageRegions.at(nextQueuedImageRegion));
}

void MainWindow::configureOcrEngine()
{
    if (processor.isBusy()) return;
    QSettings settings(QStringLiteral("YAGF"), QStringLiteral("YAGF Revival"));
    QDialog dialog(this);
    dialog.setWindowTitle(tr("OCR engine"));
    auto *layout = new QFormLayout(&dialog);
    auto *provider = new QComboBox(&dialog);
    provider->addItem(tr("Tesseract (on this computer)"));
    provider->addItem(tr("PaddleOCR (remote service)"));
    provider->addItem(tr("docTR (remote service)"));
    provider->addItem(tr("RapidOCR (remote service)"));
    provider->addItem(tr("Surya (remote service)"));
    provider->addItem(tr("Kraken (remote service)"));
    const QStringList engineIds{QStringLiteral("tesseract"), QStringLiteral("paddleocr-remote"),
                                QStringLiteral("doctr-remote"), QStringLiteral("rapidocr-remote"),
                                QStringLiteral("surya-remote"), QStringLiteral("kraken-remote")};
    const int currentEngine = engineIds.indexOf(processor.engineId());
    provider->setCurrentIndex(qMax(0, currentEngine));
    const QString paddleUrl = settings.value(QStringLiteral("ocr/paddleUrl"),
        settings.value(QStringLiteral("ocr/remoteUrl"),
            QStringLiteral("http://127.0.0.1:8765"))).toString();
    const QString doctrUrl = settings.value(QStringLiteral("ocr/doctrUrl"),
        QStringLiteral("http://127.0.0.1:8766")).toString();
    const QStringList urls{QString(), paddleUrl, doctrUrl,
        settings.value(QStringLiteral("ocr/rapidocrUrl"), QStringLiteral("http://127.0.0.1:8767")).toString(),
        settings.value(QStringLiteral("ocr/suryaUrl"), QStringLiteral("http://127.0.0.1:8768")).toString(),
        settings.value(QStringLiteral("ocr/krakenUrl"), QStringLiteral("http://127.0.0.1:8769")).toString()};
    const QStringList settingKeys{QString(), QStringLiteral("ocr/paddleUrl"), QStringLiteral("ocr/doctrUrl"),
        QStringLiteral("ocr/rapidocrUrl"), QStringLiteral("ocr/suryaUrl"), QStringLiteral("ocr/krakenUrl")};
    auto *address = new QLineEdit(urls.at(provider->currentIndex()), &dialog);
    address->setPlaceholderText(tr("http://127.0.0.1:8765"));
    auto *token = new QLineEdit(&dialog);
    token->setEchoMode(QLineEdit::Password);
    token->setPlaceholderText(tr("Optional bearer token; held only for this session"));
    layout->addRow(tr("Engine:"), provider);
    layout->addRow(tr("Service URL:"), address);
    layout->addRow(tr("Token:"), token);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(provider, &QComboBox::currentIndexChanged, &dialog, [=](int index) {
        address->setEnabled(index != 0);
        token->setEnabled(index != 0);
        address->setText(urls.at(index));
    });
    address->setEnabled(provider->currentIndex() != 0);
    token->setEnabled(provider->currentIndex() != 0);
    if (dialog.exec() != QDialog::Accepted) return;
    if (provider->currentIndex() == 0) {
        processor.setEngine(std::make_unique<processing::TesseractEngine>());
        if (languageEdit->text() == QStringLiteral("en") || languageEdit->text() == QStringLiteral("auto"))
            languageEdit->setText(QStringLiteral("eng"));
    } else {
        const QUrl url = QUrl::fromUserInput(address->text().trimmed());
        if ((url.scheme() != QStringLiteral("http") && url.scheme() != QStringLiteral("https")) ||
            url.host().isEmpty() || !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment() ||
            (!url.path().isEmpty() && url.path() != QStringLiteral("/"))) {
            showError(tr("Enter the service base URL, for example http://127.0.0.1:8765"));
            return;
        }
        const int selected = provider->currentIndex();
        using Provider = processing::RemoteOcrEngine::Provider;
        const Provider backend = selected == 2 ? Provider::DocTr :
                                 selected == 3 ? Provider::RapidOcr :
                                 selected == 4 ? Provider::Surya :
                                 selected == 5 ? Provider::Kraken : Provider::PaddleOcr;
        processor.setEngine(std::make_unique<processing::RemoteOcrEngine>(url, token->text(),
            backend));
        settings.setValue(settingKeys.at(selected), url.toString());
        if (selected != 1) languageEdit->setText(QStringLiteral("auto"));
        else if (languageEdit->text() == QStringLiteral("eng") ||
                 languageEdit->text() == QStringLiteral("auto")) languageEdit->setText(QStringLiteral("en"));
    }
    const bool tesseract = provider->currentIndex() == 0;
    languagePickerAction->setEnabled(tesseract);
    languagePickerButton->setEnabled(tesseract);
    languageEdit->setReadOnly(provider->currentIndex() > 1);
    languageEdit->setToolTip(tesseract
        ? tr("Tesseract languages, for example eng+deu. The first language is primary.")
        : provider->currentIndex() == 1
            ? tr("Language configured on the PaddleOCR service, for example en")
            : tr("This engine chooses its model vocabulary; language is auto."));
    status->setText(tr("OCR engine: %1").arg(processor.engineId()));
}

void MainWindow::chooseOcrLanguages()
{
    if (processor.engineId() != QStringLiteral("tesseract") || processor.isBusy()) return;
    QProcess process;
    process.start(QStringLiteral("tesseract"), {QStringLiteral("--list-langs")});
    if (!process.waitForFinished(5000) || process.exitStatus() != QProcess::NormalExit ||
        process.exitCode() != 0) {
        showError(tr("Could not list installed Tesseract languages."));
        return;
    }
    QStringList languages;
    const QString output = QString::fromUtf8(process.readAllStandardOutput() + process.readAllStandardError());
    const QRegularExpression code(QStringLiteral("^[A-Za-z][A-Za-z0-9_/-]*$"));
    for (const QString &row : output.split(QLatin1Char('\n'))) {
        const QString name = row.trimmed();
        if (code.match(name).hasMatch() && name != QStringLiteral("osd")) languages.append(name);
    }
    languages.removeDuplicates();
    languages.sort(Qt::CaseInsensitive);
    if (languages.isEmpty()) { showError(tr("No Tesseract recognition languages are installed.")); return; }
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Choose OCR languages"));
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(tr("Select languages to recognize together. The first code in the field is primary."),
                                 &dialog));
    auto *list = new QListWidget(&dialog);
    list->setObjectName(QStringLiteral("availableOcrLanguages"));
    list->setMinimumSize(320, 260);
    const QStringList original = languageEdit->text().split(QLatin1Char('+'), Qt::SkipEmptyParts);
    for (const QString &language : languages) {
        auto *item = new QListWidgetItem(language, list);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(original.contains(language) ? Qt::Checked : Qt::Unchecked);
    }
    layout->addWidget(list);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    const auto updateOk = [list, buttons] {
        bool checked = false;
        for (int i = 0; i < list->count(); ++i) checked |= list->item(i)->checkState() == Qt::Checked;
        buttons->button(QDialogButtonBox::Ok)->setEnabled(checked);
    };
    connect(list, &QListWidget::itemChanged, &dialog, updateOk);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    updateOk();
    if (dialog.exec() != QDialog::Accepted) return;
    QStringList selected;
    for (const QString &language : original)
        if (languages.contains(language) && list->findItems(language, Qt::MatchExactly).first()->checkState() == Qt::Checked)
            selected.append(language);
    for (int i = 0; i < list->count(); ++i)
        if (list->item(i)->checkState() == Qt::Checked && !selected.contains(list->item(i)->text()))
            selected.append(list->item(i)->text());
    languageEdit->setText(selected.join(QLatin1Char('+')));
}

void MainWindow::runBatchOcr(bool remainingOnly)
{
    if (processor.isBusy() || pageCount < 2 ||
        !originalPath.endsWith(QStringLiteral(".pdf"), Qt::CaseInsensitive) || !maybeApplyEditor()) return;
    bool hasCorrections = false;
    if (!remainingOnly) {
        for (const OcrPage &page : document.pages)
            for (const OcrWord &word : page.words) hasCorrections |= word.reviewed;
    }
    if (hasCorrections && QMessageBox::question(this, tr("Recognize all pages again"),
        tr("Matching corrections will be kept, but unmatched words may be replaced. Continue?"))
        != QMessageBox::Yes) return;
    QVector<BatchPageOptions> options;
    options.reserve(pageCount);
    for (int number = 1; number <= pageCount; ++number) {
        const OcrPage *page = document.page(number);
        if (remainingOnly && page && (!page->engineId.isEmpty() || !page->words.isEmpty())) continue;
        options.append({.number = number,
                        .language = languageEdit->text().trimmed(),
                        .preprocessing = page ? page->preprocessing : core::PreprocessSettings{}});
    }
    if (options.isEmpty()) {
        status->setText(tr("All pages have already been recognized."));
        return;
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
    const QSignalBlocker blockRegionSignals(regionList);
    wordList->clear();
    regionList->clear();
    for (QGraphicsPathItem *box : wordBoxes) {
        scene->removeItem(box);
        delete box;
    }
    wordBoxes.clear();
    for (QGraphicsRectItem *box : imageRegionBoxes) {
        scene->removeItem(box);
        delete box;
    }
    imageRegionBoxes.clear();
    displayedWords.clear();
    OcrPage *page = document.page(currentPage);
    if (!page) {
        ocrButton->setText(tr("Recognize"));
        selectedImageRegion = -1;
        selectedRegion = {};
        view->setActiveRegionIndex(-1);
        deleteRegionButton->setEnabled(false);
        regionOcrButton->setEnabled(false);
        updateSelection({});
        layoutAction->setEnabled(false);
        pdfAction->setEnabled(false);
        pdfaAction->setEnabled(false);
        return;
    }
    for (OcrWord &word : page->words) {
        const int i = displayedWords.size();
        displayedWords.append(&word);
        const bool uncertain = !word.reviewed && word.confidence >= 0 && word.confidence < 70;
        const QColor outline = uncertain ? QColor(185, 108, 10) : QColor(20, 90, 220);
        const QColor fill = uncertain ? QColor(245, 170, 45, 45) : QColor(20, 90, 220, 35);
        QPainterPath path;
        if (word.sourceBoxes.isEmpty()) path.addRect(word.bounds);
        else for (const QRect &fragment : word.sourceBoxes) path.addRect(fragment);
        auto *box = scene->addPath(path, QPen(outline, 1.5));
        box->setBrush(fill);
        box->setData(0, i);
        box->setZValue(1);
        wordBoxes.append(box);
        auto *item = new QListWidgetItem(word.text, wordList);
        if (word.reviewed) item->setForeground(QColor(0, 90, 20));
        else if (word.confidence >= 0 && word.confidence < 70)
            item->setForeground(QColor(160, 95, 0));
        item->setToolTip(tr("Confidence: %1; box: %2, %3, %4 × %5; source fragments: %6")
                         .arg(word.confidence).arg(word.bounds.x()).arg(word.bounds.y())
                         .arg(word.bounds.width()).arg(word.bounds.height()).arg(word.sourceBoxes.size()));
    }
    for (int i = 0; i < page->imageRegions.size(); ++i) {
        const ImageRegion &region = page->imageRegions.at(i);
        auto *box = scene->addRect(region.bounds, QPen(QColor(25, 135, 55), 2),
                                   QBrush(QColor(35, 160, 70, 35)));
        box->setData(1, i);
        box->setZValue(4);
        box->setAcceptedMouseButtons(Qt::NoButton);
        box->setToolTip(tr("Region %1 · drag to move; drag an edge to resize").arg(i + 1));
        imageRegionBoxes.append(box);
        regionList->addItem(tr("Region %1 · %2 × %3 px")
                            .arg(i + 1).arg(region.bounds.width()).arg(region.bounds.height()));
    }
    ocrButton->setText(page->imageRegions.isEmpty() ? tr("Recognize") : tr("Recognize regions"));
    refreshImageRegionSelection(true);
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
    selectedImageRegion = -1;
    refreshImageRegionSelection(false);
    updateSelection({index});
    view->centerOn(wordBoxes.at(index));
}

void MainWindow::selectRegion(const QRectF &bounds)
{
    if (imagePath.isEmpty() || processor.isBusy() || exportBusy) return;
    if (!maybeApplyEditor()) return;
    const QRect area = bounds.toAlignedRect().intersected(scene->sceneRect().toAlignedRect());
    if (area.width() < 8 || area.height() < 8) return;
    OcrPage edited = document.page(currentPage) ? *document.page(currentPage) : OcrPage{};
    if (edited.imageRegions.size() >= 1000) { showError(tr("This page has too many regions.")); return; }
    if (edited.width == 0) {
        edited.id = QStringLiteral("page-%1").arg(currentPage);
        edited.number = currentPage;
        edited.width = scene->sceneRect().width();
        edited.height = scene->sceneRect().height();
        edited.sourceWidth = rawImage.width();
        edited.sourceHeight = rawImage.height();
        edited.preprocessing = displayedPreprocessing;
    }
    edited.imageRegions.append({QUuid::createUuid().toString(QUuid::WithoutBraces), area});
    selectedImageRegion = edited.imageRegions.size() - 1;
    undoStack.push(new ReplacePageCommand(document, currentPage, std::move(edited), tr("Add image region")));
    activateImageRegion(selectedImageRegion);
}

void MainWindow::refreshImageRegionSelection(bool updateWords)
{
    const OcrPage *page = document.page(currentPage);
    if (!page || selectedImageRegion < 0 || selectedImageRegion >= page->imageRegions.size())
        selectedImageRegion = -1;
    selectedRegion = selectedImageRegion >= 0 ? page->imageRegions.at(selectedImageRegion).bounds : QRect{};
    view->setActiveRegionIndex(selectedImageRegion);
    {
        const QSignalBlocker blocker(regionList);
        regionList->setCurrentRow(selectedImageRegion);
    }
    for (int i = 0; i < imageRegionBoxes.size(); ++i) {
        const bool active = i == selectedImageRegion;
        imageRegionBoxes[i]->setPen(QPen(active ? QColor(210, 65, 25) : QColor(25, 135, 55),
                                         active ? 3 : 2));
        imageRegionBoxes[i]->setBrush(active ? QColor(230, 90, 35, 45) : QColor(35, 160, 70, 35));
    }
    deleteRegionButton->setEnabled(selectedImageRegion >= 0 && !processor.isBusy() && !exportBusy);
    regionOcrButton->setEnabled(selectedImageRegion >= 0 && !processor.isBusy() && !exportBusy);
    QVector<int> matches;
    if (selectedImageRegion >= 0) {
        for (int i = 0; i < displayedWords.size(); ++i)
            if (overlapsWord(*displayedWords.at(i), selectedRegion)) matches.append(i);
        status->setText(tr("Selected region %1: %2 × %3 px, %4 words")
                        .arg(selectedImageRegion + 1).arg(selectedRegion.width())
                        .arg(selectedRegion.height()).arg(matches.size()));
    } else {
        for (int i = 0; i < displayedWords.size(); ++i) matches.append(i);
    }
    if (updateWords) updateSelection(matches);
}

void MainWindow::activateImageRegion(int index)
{
    const OcrPage *page = document.page(currentPage);
    if (!page || index < 0 || index >= page->imageRegions.size() || !maybeApplyEditor()) return;
    selectedImageRegion = index;
    refreshImageRegionSelection(true);
}

void MainWindow::moveImageRegion(int index, const QRectF &bounds)
{
    const OcrPage *page = document.page(currentPage);
    if (!page || index < 0 || index >= page->imageRegions.size() || processor.isBusy() ||
        exportBusy || !maybeApplyEditor()) return;
    const QRect area = bounds.toAlignedRect().intersected(scene->sceneRect().toAlignedRect());
    if (area.width() < 8 || area.height() < 8 || area == page->imageRegions.at(index).bounds) return;
    OcrPage edited = *page;
    edited.imageRegions[index].bounds = area;
    selectedImageRegion = index;
    undoStack.push(new ReplacePageCommand(document, currentPage, std::move(edited), tr("Move image region")));
}

void MainWindow::deleteImageRegion(int index)
{
    const OcrPage *page = document.page(currentPage);
    if (!page || index < 0 || index >= page->imageRegions.size() || processor.isBusy() ||
        exportBusy || !maybeApplyEditor()) return;
    OcrPage edited = *page;
    edited.imageRegions.removeAt(index);
    selectedImageRegion = -1;
    undoStack.push(new ReplacePageCommand(document, currentPage, std::move(edited), tr("Delete image region")));
}

void MainWindow::refreshReviewLabel()
{
    QString reviewLabel = tr("Review word…");
    if (selectedWords.size() == 1) {
        if (const auto *page = document.page(currentPage)) {
            if (!correctionMemory) correctionMemory.emplace(document, similarSymbolsAction->isChecked());
            const auto suggestions = correctionMemory->suggestions(*page, selectedWords.first());
            if (!suggestions.isEmpty()) {
                QString reading = reviewWordButton->fontMetrics().elidedText(
                    suggestions.first().replacement, Qt::ElideMiddle, 180);
                reviewLabel = tr("Review suggestion: %1…").arg(reading.replace('&', "&&"));
            }
        }
    }
    reviewWordButton->setText(reviewLabel);
}

void MainWindow::updateSelection(const QVector<int> &indices)
{
    selectedWords = indices;
    refreshReviewLabel();
    reviewWordButton->setEnabled(!processor.isBusy() && !exportBusy && !imagePath.isEmpty() &&
                               indices.size() == 1);
    if (createRegionAction) createRegionAction->setEnabled(!processor.isBusy() && !exportBusy && !indices.isEmpty() &&
        document.page(currentPage) && indices.size() < document.page(currentPage)->words.size());
    QSet<int> selectedSet;
    for (int index : indices) selectedSet.insert(index);
    const QSignalBlocker listBlocker(wordList);
    wordList->clearSelection();
    for (int i = 0; i < wordBoxes.size(); ++i) {
        const bool selected = selectedSet.contains(i);
        const bool uncertain = !displayedWords.at(i)->reviewed &&
                               displayedWords.at(i)->confidence >= 0 && displayedWords.at(i)->confidence < 70;
        wordBoxes[i]->setPen(QPen(selected ? QColor(215, 65, 25) :
                                   uncertain ? QColor(185, 108, 10) : QColor(20, 90, 220),
                                   selected ? 2.5 : 1.5,
                                   displayedWords.at(i)->sourceBoxes.isEmpty() ? Qt::DashLine : Qt::SolidLine));
        wordBoxes[i]->setBrush(selected ? QColor(235, 95, 30, 50) :
                                   uncertain ? QColor(245, 170, 45, 45) : QColor(20, 90, 220, 35));
        if (selected) wordList->item(i)->setSelected(true);
    }
    const QSignalBlocker editorBlocker(selectionEditor);
    selectionEditor->clear();
    editorPending = false;
    const bool multipleBlocks = indices.size() > 1 && std::any_of(indices.cbegin(), indices.cend(),
        [this](int i) { return displayedWords.at(i)->layoutBlock; });
    selectionEditor->setReadOnly(multipleBlocks);
    applyButton->setEnabled(!processor.isBusy() && !exportBusy && !indices.isEmpty() && !multipleBlocks);
    selectionLabel->setText(multipleBlocks ? tr("Select one layout block to edit") : indices.isEmpty() ? tr("Recognized text") :
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

void MainWindow::reviewSelectedWord()
{
    if (processor.isBusy() || exportBusy || imagePath.isEmpty() || selectedWords.size() != 1 ||
        !maybeApplyEditor() || selectedWords.size() != 1) return;
    const OcrPage *page = document.page(currentPage);
    if (!page || selectedWords.first() < 0 || selectedWords.first() >= page->words.size()) return;
    const int index = selectedWords.first();
    const OcrWord &word = page->words.at(index);
    const QImage image(imagePath);
    if (image.isNull()) { showError(tr("Could not load the page image for review.")); return; }
    const QRect bounds = word.bounds.intersected(image.rect());
    if (bounds.isEmpty()) { showError(tr("This word has no usable source image box.")); return; }
    const QRect cropRect = bounds.adjusted(-qMax(45, bounds.width() / 2),
        -qMax(25, bounds.height()), qMax(45, bounds.width() / 2),
        qMax(25, bounds.height())).intersected(image.rect());
    QStringList context;
    for (int i = qMax(0, index - 6); i <= qMin(page->words.size() - 1, index + 6); ++i)
        context.append(i == index ? QStringLiteral("[%1]").arg(page->words.at(i).text)
                                  : page->words.at(i).text);
    AiReviewDialog dialog(word.text, context.join(QLatin1Char(' ')), image.copy(cropRect),
                          bounds.translated(-cropRect.topLeft()), aiReviewToken, this);
    if (!correctionMemory) correctionMemory.emplace(document, similarSymbolsAction->isChecked());
    dialog.setBookSuggestions(correctionMemory->suggestions(*page, index));
    const int result = dialog.exec();
    aiReviewToken = dialog.sessionToken();
    if (result != QDialog::Accepted) return;
    QTextCursor cursor(selectionEditor->document());
    cursor.select(QTextCursor::Document);
    cursor.insertText(dialog.correction(), selectionEditor->currentCharFormat());
    applyCorrection();
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
    if (selectionEditor->isReadOnly()) { editorPending = false; return true; }
    const QString plain = selectionEditor->toPlainText();
    if (selectedWords.size() == 1 && displayedWords.at(selectedWords.first())->layoutBlock &&
        !plain.trimmed().isEmpty()) {
        auto edited = *document.page(currentPage);
        auto &block = edited.words[selectedWords.first()];
        block.text = plain;
        block.reviewed = true;
        QTextCursor cursor(selectionEditor->document());
        cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
        block.bold = cursor.charFormat().fontWeight() >= QFont::Bold;
        block.italic = cursor.charFormat().fontItalic();
        const QString id = block.id;
        editorPending = false;
        undoStack.push(new ReplaceTextCommand(document, currentPage, std::move(edited)));
        updateSelection(indicesForIds({id}));
        return true;
    }
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
    startExport(path, archive ? ExportFormat::ArchivalPdf : ExportFormat::Pdf);
}

void MainWindow::exportScientificBook()
{
    if (processor.isBusy() || exportBusy || !maybeApplyEditor()) return;
    if (!document.wordCount()) { showError(tr("Recognize the document before exporting its reading order.")); return; }
    if (document.pages.size() < document.totalPages &&
        QMessageBox::question(this, tr("Pages without OCR"),
            tr("Export only the recognized pages?"), QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel) != QMessageBox::Yes) return;
    chooseExportPath(tr("Export scientific book"), tr("HTML (*.html)"), "html", ExportFormat::ScientificBook);
}

void MainWindow::exportVerifiedSamples()
{
    if (processor.isBusy() || exportBusy || !maybeApplyEditor()) return;
    const bool hasExamples = std::any_of(document.pages.cbegin(), document.pages.cend(), [](const auto &page) {
        return std::any_of(page.words.cbegin(), page.words.cend(), core::isVerifiedCorrection);
    });
    if (!hasExamples) { showError(tr("No verified single-word corrections to export.")); return; }
    chooseExportPath(tr("Export verified corrections"), tr("JSON Lines (*.jsonl)"), "jsonl", ExportFormat::Samples);
}

void MainWindow::chooseExportPath(const QString &title, const QString &filter, const QString &suffix, ExportFormat format)
{
    QFileDialog dialog(this, title, {}, filter);
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setDefaultSuffix(suffix);
    if (dialog.exec() != QDialog::Accepted) return;
    const QString path = dialog.selectedFiles().value(0);
    if (path.isEmpty()) return;
    if (QFileInfo(path) == QFileInfo(originalPath) || (!projectPath.isEmpty() && QFileInfo(path) == QFileInfo(projectPath))) {
        showError(tr("Choose a destination separate from the source and project files."));
        return;
    }
    startExport(path, format);
}

void MainWindow::startExport(const QString &path, ExportFormat format)
{
    exportPath = path;
    exportCancel = std::make_shared<std::atomic_bool>(false);
    exportBusy = true;
    setBusy(true);
    status->setText(tr("Preparing export…"));
    const OcrDocument snapshot = document;
    const auto cancel = exportCancel;
    exportWatcher.setFuture(QtConcurrent::run([this, snapshot, path, format, cancel] {
        const auto progress = [this](int completed, int total) {
            QMetaObject::invokeMethod(this, [this, completed, total] {
                if (exportBusy) status->setText(tr("Export: %1 of %2 pages").arg(completed).arg(total));
            }, Qt::QueuedConnection);
        };
        if (format == ExportFormat::ScientificBook)
            return processing::ScientificBookExporter::write(snapshot, path, *cancel, progress);
        if (format == ExportFormat::Samples)
            return processing::VerifiedSampleExporter::write(snapshot, path, *cancel, progress);
        return processing::PdfExporter::write(snapshot, path, format == ExportFormat::ArchivalPdf, *cancel, progress);
    }));
}

} // namespace yagf::ui

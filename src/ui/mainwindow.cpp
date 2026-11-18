#include "pdfexporter.h"
#include "scientificbookexporter.h"
#include "verifiedsampleexporter.h"
#include "mainwindow.h"
#include "blockminiaturelist.h"
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
#include "regiongeometry.h"
#include "spellhighlighter.h"
#include "pagesource.h"

#include <QCloseEvent>
#include <QApplication>
#include <QClipboard>
#include <QCheckBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QActionGroup>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFontComboBox>
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
#include <QInputDialog>
#include <QKeyEvent>
#include <QIcon>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QLocale>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QMenuBar>
#include <QMenu>
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
#include <QPlainTextEdit>
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
#include <QtConcurrentRun>
#include <QUuid>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <utility>
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
QIcon recognizeActionIcon()
{
    return QIcon::fromTheme(QStringLiteral("document-scan"),
                           QApplication::style()->standardIcon(QStyle::SP_FileDialogContentsView));
}

QIcon removeBlockIcon()
{
    return QIcon::fromTheme(QStringLiteral("edit-delete"),
                           QApplication::style()->standardIcon(QStyle::SP_TrashIcon));
}

QIcon textStyleActionIcon(bool bold)
{
    QPixmap pixmap(32, 32);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::TextAntialiasing);
    QFont font = QApplication::font();
    font.setPixelSize(30);
    font.setBold(bold);
    font.setItalic(!bold);
    painter.setFont(font);
    painter.setPen(QApplication::palette().color(QPalette::ButtonText));
    painter.drawText(pixmap.rect(), Qt::AlignCenter, bold ? QStringLiteral("B") : QStringLiteral("I"));
    painter.end();
    return QIcon::fromTheme(bold ? QStringLiteral("format-text-bold") : QStringLiteral("format-text-italic"),
                            QIcon(pixmap));
}

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

void carryCorrections(OcrPage &fresh, const OcrPage *oldPage, bool estimateFormatting)
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
            if (old.reviewed || !estimateFormatting) {
                word.bold = old.bold;
                word.italic = old.italic;
                word.fontFamily = old.fontFamily;
                word.fontSizePt = old.fontSizePt;
                word.sourceFontSizePt = old.sourceFontSizePt;
                word.sourceFontSizePx = old.sourceFontSizePx;
            }
            word.sourceWordIds = old.sourceWordIds;
            word.sourceBoxes = old.sourceBoxes;
            if (old.reviewed) {
                word.text = old.text;
                word.reviewed = true;
                word.readingEngine = old.readingEngine;
                word.alternative = {};
            }
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
    std::ranges::stable_sort(groups, [&](const auto &a, const auto &b) {
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
        const auto point = mapToScene(event->pos());
        const auto limits = scene()->sceneRect();
        regionAnchor = QPointF(qBound(limits.left(), qreal(qRound(point.x())), limits.right()),
                               qBound(limits.top(), qreal(qRound(point.y())), limits.bottom()));
        drawingRegion = {};
        rubberBand->setGeometry(QRect(event->pos(), QSize()));
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

QVector<QRectF> PageView::regionObstacles() const
{
    QVector<QRectF> obstacles;
    for (auto *candidate : scene()->items())
        if (auto *box = qgraphicsitem_cast<QGraphicsRectItem *>(candidate);
            box && box != editingRegionItem && box->data(1).isValid()) obstacles.append(box->rect());
    return obstacles;
}

void PageView::updateRegionDrag(const QPoint &position)
{
    if (editingRegionItem) {
        const QPointF delta = mapToScene(position) - editStartScene;
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
        editingRegionItem->setRect(core::constrainRegion(editStartRect, next, regionObstacles()));
    } else {
        const auto point = mapToScene(position);
        const auto limits = scene()->sceneRect();
        const QPointF end(qBound(limits.left(), qreal(qRound(point.x())), limits.right()),
                          qBound(limits.top(), qreal(qRound(point.y())), limits.bottom()));
        drawingRegion = core::constrainRegion(QRectF(regionAnchor, QSizeF(0, 0)),
            QRectF(regionAnchor, end).normalized(), regionObstacles());
        rubberBand->setGeometry(mapFromScene(QRectF(drawingRegion)).boundingRect());
    }
}

void PageView::mouseMoveEvent(QMouseEvent *event)
{
    if (regionMode && (editingRegionItem || rubberBand->isVisible())) {
        updateRegionDrag(event->pos());
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
        updateRegionDrag(event->pos());
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
        updateRegionDrag(event->pos());
        rubberBand->hide();
        if (drawingRegion.width() >= 8 && drawingRegion.height() >= 8)
            emit regionSelected(QRectF(drawingRegion));
        event->accept();
        return;
    }
    QGraphicsView::mouseReleaseEvent(event);
}

void PageView::contextMenuEvent(QContextMenuEvent *event)
{
    int index = event->reason() == QContextMenuEvent::Keyboard ? activeRegionIndex : -1;
    if (index < 0)
        for (auto *hit : items(event->pos()))
            if (auto *box = qgraphicsitem_cast<QGraphicsRectItem *>(hit); box && box->data(1).isValid()) {
                index = box->data(1).toInt();
                break;
            }
    if (index >= 0) {
        emit regionContextMenuRequested(index, event->globalPos());
        event->accept();
    } else QGraphicsView::contextMenuEvent(event);
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
    fileMenu->addAction(tr("Open image, PDF or DjVu…"), this, &MainWindow::openFile);
    auto *openAction = toolbar->addAction(style()->standardIcon(QStyle::SP_DialogOpenButton),
                                          tr("Open"), this, &MainWindow::openFile);
    openAction->setToolTip(tr("Open an image, PDF or DjVu"));
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
    pageSpin->setToolTip(tr("Document page number"));
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
    preserveFormattingAction = toolsMenu->addAction(tr("Preserve formatting"));
    preserveFormattingAction->setObjectName(QStringLiteral("preserveFormattingAction"));
    preserveFormattingAction->setCheckable(true);
    preserveFormattingAction->setToolTip(tr("Estimate the closest installed font, bold, italic and size during recognition. "
        "Latin printed text is supported; paragraph OCR blocks receive one estimated style."));
    connect(preserveFormattingAction, &QAction::toggled, this, [this](bool enabled) {
        document.preserveFormatting = enabled;
        if (!originalPath.isEmpty()) { externalDirty = true; refreshDirty(); }
    });
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
    toolsMenu->addAction(tr("Specialist OCR…"), this, &MainWindow::configureSpecialists);
    documentContextAction = toolsMenu->addAction(tr("Document context…"), this, &MainWindow::configureDocumentContext);
    documentContextAction->setObjectName(QStringLiteral("documentContextAction"));
    documentContextAction->setEnabled(false);
    nextSpecialistAction = toolsMenu->addAction(tr("Review next specialist reading…"), this, &MainWindow::reviewNextSpecialist);
    languagePickerAction = toolsMenu->addAction(tr("Choose OCR languages…"), this,
                                                &MainWindow::chooseOcrLanguages);
    layoutAction->setEnabled(false);
    createRegionAction->setEnabled(false);
    batchAction->setEnabled(false);
    ocrButton = new QPushButton(tr("Recognize"), this);
    ocrButton->setIcon(recognizeActionIcon());
    ocrButton->setIconSize(QSize(20, 20));
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
    formattedPdfAction = fileMenu->addAction(tr("Export formatted PDF…"), this, [this] { exportPdf(false, true); });
    formattedPdfAction->setObjectName(QStringLiteral("exportFormattedPdfAction"));
    formattedPdfAction->setToolTip(tr("Recreate corrected text using matching fonts in the original page blocks."));
    formattedPdfAction->setEnabled(false);
    pdfaAction = fileMenu->addAction(tr("Export archival PDF/A…"), this, [this] { exportPdf(true); });
    convertDjVuAction = fileMenu->addAction(tr("Convert DjVu to PDF…"), this, &MainWindow::convertDjVuToPdf);
    convertDjVuAction->setObjectName(QStringLiteral("convertDjVuAction"));
    convertDjVuAction->setEnabled(false);
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
    auto *blockMiniatures = new BlockMiniatureList(navigationPane);
    regionList = blockMiniatures;
    regionList->setObjectName(QStringLiteral("imageRegions"));
    regionList->setToolTip(tr("Right-click a block miniature for recognition, language selection, or removal. "
        "Select a block and press Shift+F10 to open its menu from the keyboard."));
    navigationLayout->addWidget(regionList, 2);
    deleteRegionButton = new QPushButton(tr("Delete region"), navigationPane);
    deleteRegionButton->setIcon(removeBlockIcon());
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
    textPanel->setMinimumWidth(450);
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
    auto *editorSplitter = new QSplitter(Qt::Vertical, textPanel);
    editorSplitter->setObjectName(QStringLiteral("textEditorSplitter"));
    editorSplitter->setChildrenCollapsible(false);
    editorSplitter->setHandleWidth(5);
    textLayout->addWidget(editorSplitter, 1);
    selectionEditor = new QTextEdit(editorSplitter);
    selectionEditor->setObjectName(QStringLiteral("recognizedTextEditor"));
    selectionEditor->setPlaceholderText(tr("Select words on the page or drag a region, then edit here."));
    selectionEditor->setAcceptRichText(true);
    selectionEditor->setMinimumHeight(280);
    spellHighlighter = new SpellHighlighter(selectionEditor->document());
    auto *wordPanel = new QWidget(editorSplitter);
    auto *wordLayout = new QVBoxLayout(wordPanel);
    wordLayout->setContentsMargins(0, 8, 0, 0);
    wordLayout->setSpacing(10);
    auto *editButtons = new QHBoxLayout();
    applyButton = new QPushButton(tr("Apply correction"), textPanel);
    applyButton->setObjectName(QStringLiteral("applyButton"));
    applyButton->setEnabled(false);
    regionOcrButton = new QPushButton(tr("Recognize region"), textPanel);
    regionOcrButton->setIcon(recognizeActionIcon());
    regionOcrButton->setObjectName(QStringLiteral("recognizeRegionButton"));
    regionOcrButton->setEnabled(false);
    editButtons->addWidget(applyButton);
    editButtons->addWidget(regionOcrButton);
    wordLayout->addLayout(editButtons);
    reviewWordButton = new QPushButton(tr("Review word…"), textPanel);
    reviewWordButton->setObjectName(QStringLiteral("reviewWordButton"));
    reviewWordButton->setEnabled(false);
    reviewWordButton->setToolTip(tr("Inspect an enlarged source crop and request contextual reading suggestions"));
    wordLayout->addWidget(reviewWordButton);
    auto *wordsHeading = new QLabel(tr("WORDS ON THIS PAGE"), textPanel);
    wordsHeading->setObjectName(QStringLiteral("sectionHeading"));
    wordLayout->addWidget(wordsHeading);
    wordList = new QListWidget(textPanel);
    wordList->setObjectName(QStringLiteral("recognizedWords"));
    wordList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    wordList->setToolTip(tr("Select a word to locate it on the scan and edit it above."));
    wordList->setMinimumHeight(80);
    wordLayout->addWidget(wordList, 1);
    editorSplitter->setStretchFactor(0, 4);
    editorSplitter->setStretchFactor(1, 1);
    editorSplitter->setSizes({480, 200});
    auto *boldAction = formatToolbar->addAction(textStyleActionIcon(true), tr("Bold"));
    boldAction->setShortcut(QKeySequence::Bold);
    boldAction->setToolTip(tr("Bold"));
    qobject_cast<QToolButton *>(formatToolbar->widgetForAction(boldAction))->setToolButtonStyle(Qt::ToolButtonIconOnly);
    connect(boldAction, &QAction::triggered, this, [this] {
        QTextCharFormat format;
        const bool bold = selectionEditor->currentCharFormat().fontWeight() >= QFont::Bold;
        format.setFontWeight(bold ? QFont::Normal : QFont::Bold);
        selectionEditor->mergeCurrentCharFormat(format);
        editorPending = true;
    });
    auto *italicAction = formatToolbar->addAction(textStyleActionIcon(false), tr("Italic"));
    italicAction->setShortcut(QKeySequence::Italic);
    italicAction->setToolTip(tr("Italic"));
    qobject_cast<QToolButton *>(formatToolbar->widgetForAction(italicAction))->setToolButtonStyle(Qt::ToolButtonIconOnly);
    connect(italicAction, &QAction::triggered, this, [this] {
        QTextCharFormat format;
        format.setFontItalic(!selectionEditor->currentCharFormat().fontItalic());
        selectionEditor->mergeCurrentCharFormat(format);
        editorPending = true;
    });
    auto *fontPicker = new QFontComboBox(formatToolbar);
    fontPicker->setObjectName(QStringLiteral("textFontFamily"));
    fontPicker->setMaximumWidth(160);
    fontPicker->setToolTip(tr("Font family for selected editor text"));
    formatToolbar->addWidget(fontPicker);
    connect(fontPicker, &QFontComboBox::activated, this, [this, fontPicker](int) {
        QTextCharFormat format;
        format.setFontFamilies({fontPicker->currentFont().family()});
        selectionEditor->mergeCurrentCharFormat(format);
        editorPending = true;
    });
    connect(selectionEditor, &QTextEdit::currentCharFormatChanged, fontPicker,
        [fontPicker](const QTextCharFormat &format) {
            const QSignalBlocker blocker(fontPicker);
            fontPicker->setCurrentFont(format.font());
        });
    formatToolbar->addSeparator();
    spellCheckAction = formatToolbar->addAction(tr("Spell check"));
    spellCheckAction->setObjectName(QStringLiteral("spellCheckAction"));
    spellCheckAction->setCheckable(true);
    spellCheckAction->setChecked(settings.value(QStringLiteral("spell/enabled"), true).toBool());
    spellHighlighter->setEnabled(spellCheckAction->isChecked());
    spellLanguage = new QComboBox(formatToolbar);
    spellLanguage->setObjectName(QStringLiteral("spellLanguage"));
    spellLanguage->setAccessibleName(tr("Spelling language"));
    spellLanguage->addItem(tr("Automatic (block language)"), QString());
    const auto dictionaries = spellHighlighter->availableDictionaries();
    for (const auto &code : dictionaries) {
        if (code.endsWith(QStringLiteral("_frami")) && dictionaries.contains(code.chopped(6))) continue;
        spellLanguage->addItem(tr("%1 (%2)").arg(QLocale::languageToString(QLocale(code).language()), code), code);
    }
    spellLanguage->setMinimumWidth(210);
    spellLanguage->setMaximumWidth(240);
    spellLanguage->setMinimumContentsLength(20);
    spellLanguage->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    const int dictionary = spellLanguage->findData(settings.value(QStringLiteral("spell/language"), QString()).toString());
    spellLanguage->setCurrentIndex(qMax(0, dictionary));
    formatToolbar->addWidget(spellLanguage);
    connect(spellCheckAction, &QAction::toggled, this, [this](bool checked) {
        const QSignalBlocker blocker(selectionEditor);
        spellHighlighter->setEnabled(checked);
        QSettings(QStringLiteral("YAGF"), QStringLiteral("YAGF Revival")).setValue(QStringLiteral("spell/enabled"), checked);
        refreshSpellLanguage();
    });
    connect(spellLanguage, &QComboBox::currentIndexChanged, this, [this] {
        QSettings(QStringLiteral("YAGF"), QStringLiteral("YAGF Revival"))
            .setValue(QStringLiteral("spell/language"), spellLanguage->currentData().toString());
        refreshSpellLanguage();
    });
    connect(languageEdit, &QLineEdit::textChanged, this, &MainWindow::refreshSpellLanguage);
    refreshSpellLanguage();
    splitter->addWidget(navigationPane);
    splitter->addWidget(view);
    splitter->addWidget(textPanel);
    splitter->setStretchFactor(1, 1);
    splitter->setStretchFactor(2, 1);
    splitter->setSizes({210, 520, 550});
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
    connect(view, &PageView::regionContextMenuRequested, this, &MainWindow::showBlockContextMenu);
    connect(blockMiniatures, &BlockMiniatureList::blockContextMenuRequested, this, &MainWindow::showBlockContextMenu);
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
    status = new QLabel(tr("Open an image, PDF or DjVu to begin."), this);
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
    connect(&processor, &DocumentProcessor::documentOpened, this, [this](int count) {
        if (!projectPath.isEmpty() && document.totalPages != count) {
            // Version 1 projects described only the first rendered page.
            if (document.totalPages == 1 && document.pages.size() == 1 && document.pages.first().number == 1)
                markDirty();
            else {
                showError(tr("The source document page count changed since this project was saved."));
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
    connect(&processor, &DocumentProcessor::specialistWarning, this, [this](const QString &message) {
        statusBar()->showMessage(message, 30000);
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
        if (region.isEmpty()) carryCorrections(newPage, oldPage, document.preserveFormatting);
        else if (oldPage) {
            for (OcrWord &word : newPage.words) {
                for (const OcrWord &old : oldPage->words) {
                    if (old.bounds == word.bounds && old.originalText == word.originalText) {
                        word.id = old.id;
                        if (old.reviewed || !document.preserveFormatting) {
                            word.bold = old.bold;
                            word.italic = old.italic;
                            word.fontFamily = old.fontFamily;
                            word.fontSizePt = old.fontSizePt;
                            word.sourceFontSizePt = old.sourceFontSizePt;
                            word.sourceFontSizePx = old.sourceFontSizePx;
                        }
                        word.sourceWordIds = old.sourceWordIds;
                        word.sourceBoxes = old.sourceBoxes;
                        if (old.reviewed) {
                            word.text = old.text;
                            word.reviewed = true;
                            word.readingEngine = old.readingEngine;
                            word.alternative = {};
                        }
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
            qsizetype insertion = -1;
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
        carryCorrections(page, oldPage, document.preserveFormatting);
        const QString language = page.language;
        const QString engineId = page.engineId;
        undoStack.push(new ReplacePageCommand(document, number, std::move(page), tr("Recognize page")));
        document.engine = engineId;
        document.language = language;
        document.totalPages = pageCount;
        if (number == currentPage) showDocument();
    });
    connect(&processor, &DocumentProcessor::batchProgress, this, [this](qsizetype completed, qsizetype total) {
        status->setText(tr("Batch OCR: %1 of %2 pages complete").arg(completed).arg(total));
    });
    connect(&processor, &DocumentProcessor::batchFinished, this, [this] {
        status->setText(tr("Batch OCR complete"));
    });
    connect(this, &MainWindow::exportProgress, this, [this](qsizetype completed, qsizetype total) {
        if (exportBusy) status->setText(tr("Export: %1 of %2 pages").arg(completed).arg(total));
    }, Qt::QueuedConnection);
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
    const auto *activePage = document.page(currentPage);
    const bool textRegion = !activePage || selectedImageRegion < 0 || selectedImageRegion >= activePage->imageRegions.size() ||
                            !activePage->imageRegions[selectedImageRegion].notText;
    regionOcrButton->setEnabled(!blocked && textRegion && !imagePath.isEmpty() && !selectedRegion.isEmpty());
    reviewWordButton->setEnabled(!blocked && !imagePath.isEmpty() && canReviewSelection());
    regionList->setEnabled(!blocked);
    deleteRegionButton->setEnabled(!blocked && selectedImageRegion >= 0);
    improveAction->setEnabled(!blocked && !rawImage.isNull());
    batchAction->setEnabled(!blocked && pageCount > 1 && processing::isPagedSource(originalPath));
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
    if (preserveFormattingAction) preserveFormattingAction->setEnabled(available);
    if (formattedPdfAction) formattedPdfAction->setEnabled(available && document.wordCount() > 0);
    if (convertDjVuAction) convertDjVuAction->setEnabled(available && !imagePath.isEmpty() &&
        processing::pageSourceKind(originalPath) == processing::PageSourceKind::DjVu);
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
        processing::isPagedSource(originalPath));
    if (remainingAction) remainingAction->setEnabled(batchAction->isEnabled());
    if (documentContextAction) documentContextAction->setEnabled(available && !originalPath.isEmpty());
    if (nextSpecialistAction) nextSpecialistAction->setEnabled(available && !imagePath.isEmpty() &&
        document.page(currentPage) && !document.page(currentPage)->words.isEmpty());
    if (layoutAction) layoutAction->setEnabled(available && document.page(currentPage) &&
        !document.page(currentPage)->regions.isEmpty());
    if (createRegionAction) createRegionAction->setEnabled(available && !selectedWords.isEmpty() &&
        document.page(currentPage) && selectedWords.size() < document.page(currentPage)->words.size());
}

void MainWindow::showError(const QString &message)
{
    status->setText(message);
    QMessageBox::warning(this, tr("YAGF 2"), message);
}

void MainWindow::openFile()
{
    const QString path = QFileDialog::getOpenFileName(this, tr("Open document"), {},
        tr("Documents (*.pdf *.djvu *.djv *.png *.jpg *.jpeg *.tif *.tiff *.bmp *.webp *.avif *.heic);;All files (*)"));
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
        const auto image = qvariant_cast<QImage>(mime->imageData());
        if (image.isNull()) { showError(tr("The clipboard image could not be read.")); return; }
        const QString path = saveImportedImage(image);
        if (path.isEmpty()) return;
        loadSource(path);
        if (originalPath != path) QFile::remove(path);
        return;
    }
    showError(tr("Drop a local image, PDF or DjVu, or paste an image from the clipboard."));
}

QString MainWindow::saveImportedImage(const QImage &image)
{
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/imports");
    if (!QDir().mkpath(directory)) { showError(tr("Could not create the image import folder.")); return {}; }
    QString path = directory + QLatin1Char('/') +
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
    if (!source.isFile() || (!processing::isPagedSource(path) &&
                              !QImageReader(path).canRead())) {
        showError(tr("This file is not a readable image, PDF or DjVu: %1").arg(path));
        return;
    }
    if (!maybeSave()) return;
    processor.resetSource();
    originalPath = QFileInfo(path).absoluteFilePath();
    projectPath.clear();
    document = {};
    document.preserveFormatting = preserveFormattingAction->isChecked();
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
    if (processing::isPagedSource(originalPath)) {
        status->setText(tr("Reading document…"));
        processor.openDocument(originalPath);
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
    {
        const QSignalBlocker blocker(preserveFormattingAction);
        preserveFormattingAction->setChecked(document.preserveFormatting);
    }
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
    if (processing::isPagedSource(originalPath)) {
        status->setText(tr("Reading project source document…"));
        processor.openDocument(originalPath);
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
    projectPath = std::move(path);
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
    setWindowTitle(tr("YAGF 2 — %1%2").arg(name, dirty ? QStringLiteral(" *") : QString()));
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
    imagePath = std::move(recognitionPath);
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
    if (processing::isPagedSource(originalPath)) {
        status->setText(tr("Rendering page %1 of %2…").arg(number).arg(pageCount));
        processor.renderDocumentPage(number);
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
                    hasCorrections |= !region.notText && overlapsWord(word, region.bounds);
        if (hasCorrections && QMessageBox::question(this, tr("Recognize image regions"),
            tr("Some selected regions contain corrected text. New OCR may replace unmatched corrections. Continue?"))
            != QMessageBox::Yes) return;
        queuedImageRegions.clear();
        for (const ImageRegion &region : page->imageRegions)
            if (!region.notText) queuedImageRegions.append(region);
        if (queuedImageRegions.isEmpty()) {
            status->setText(tr("All blocks are marked Not a text."));
            return;
        }
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
        const qsizetype count = queuedImageRegions.size();
        queuedImageRegions.clear();
        status->setText(tr("Recognized %1 image regions").arg(count));
        return;
    }
    if (processor.isBusy() || imagePath.isEmpty()) return;
    status->setText(tr("Recognizing region %1 of %2…")
                    .arg(nextQueuedImageRegion + 1).arg(queuedImageRegions.size()));
    const auto &region = queuedImageRegions.at(nextQueuedImageRegion);
    processor.recognizeRegion(imagePath, blockLanguage(region), currentPage, region.bounds);
}

bool MainWindow::usePaddleVlService(const QUrl &url)
{
    if (processor.isBusy() || exportBusy || (url.scheme() != "http" && url.scheme() != "https") ||
        url.host().isEmpty() || !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment() ||
        (!url.path().isEmpty() && url.path() != "/")) return false;
    processor.setEngine(std::make_unique<processing::RemoteOcrEngine>(url));
    languageEdit->setText(QStringLiteral("auto"));
    languageEdit->setReadOnly(false);
    languageEdit->setToolTip(tr("PaddleOCR-VL uses its multilingual model vocabulary."));
    languagePickerAction->setEnabled(false);
    languagePickerButton->setEnabled(false);
    QSettings(QStringLiteral("YAGF"), QStringLiteral("YAGF Revival"))
        .setValue(QStringLiteral("ocr/paddleUrl"), url.toString());
    status->setText(tr("OCR engine: PaddleOCR-VL · %1").arg(url.toString()));
    return true;
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
    const auto currentEngine = static_cast<int>(engineIds.indexOf(processor.engineId()));
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

void MainWindow::configureSpecialists()
{
    if (processor.isBusy() || exportBusy) return;
    QSettings settings(QStringLiteral("YAGF"), QStringLiteral("YAGF Revival"));
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Specialist OCR"));
    auto *form = new QFormLayout(&dialog);
    auto *info = new QLabel(tr("During Tesseract OCR, send matching line images to these services. "
                               "You can review each alternative against the scan."), &dialog);
    info->setWordWrap(true);
    form->addRow(info);
    struct Fields { QCheckBox *enabled; QLineEdit *address; QLineEdit *token; };
    const auto addService = [&](const QString &label, const QString &key, const QString &fallback,
                                const SpecialistService &service) {
        Fields fields{new QCheckBox(label, &dialog),
                      new QLineEdit(service.url.isEmpty() ? settings.value(key, fallback).toString() : service.url, &dialog),
                      new QLineEdit(service.token, &dialog)};
        connect(fields.address, &QLineEdit::textEdited, fields.token, &QLineEdit::clear);
        fields.enabled->setChecked(service.enabled);
        fields.token->setEchoMode(QLineEdit::Password);
        fields.token->setPlaceholderText(tr("Optional session token"));
        form->addRow(fields.enabled);
        form->addRow(tr("Service URL"), fields.address);
        form->addRow(tr("Bearer token"), fields.token);
        return fields;
    };
    const auto math = addService(tr("Formulas with Surya"), "ocr/suryaUrl", "http://127.0.0.1:8768", formulaService);
    const auto prose = addService(tr("Uncertain text with docTR"), "ocr/doctrUrl", "http://127.0.0.1:8766", proseService);
    auto *error = new QLabel(&dialog);
    form->addRow(error);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        for (const auto &fields : {math, prose}) {
            const QUrl url(fields.address->text().trimmed());
            if (fields.enabled->isChecked() && ((url.scheme() != "http" && url.scheme() != "https") ||
                url.host().isEmpty() || !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment() ||
                (!url.path().isEmpty() && url.path() != "/"))) {
                error->setText(tr("Enter an HTTP(S) service base URL for each enabled engine."));
                return;
            }
        }
        dialog.accept();
    });
    if (dialog.exec() != QDialog::Accepted) return;
    using Remote = processing::RemoteOcrEngine;
    const auto service = [&](const Fields &fields, SpecialistService &state, const QString &key,
                             Remote::Provider provider) -> std::unique_ptr<Remote> {
        state = {fields.enabled->isChecked(), fields.address->text().trimmed(), fields.token->text()};
        if (!state.enabled) return {};
        const QUrl url(state.url);
        settings.setValue(key, url.toString());
        return std::make_unique<Remote>(url, state.token, provider);
    };
    processor.setSpecialists(service(math, formulaService, "ocr/suryaUrl", Remote::Provider::Surya),
                             service(prose, proseService, "ocr/doctrUrl", Remote::Provider::DocTr));
}

void MainWindow::configureDocumentContext()
{
    if (processor.isBusy() || exportBusy || originalPath.isEmpty() || !maybeApplyEditor()) return;
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("documentContextDialog"));
    dialog.setWindowTitle(tr("Document context"));
    dialog.resize(600, 380);
    auto *layout = new QVBoxLayout(&dialog);
    auto *explanation = new QLabel(tr("Describe the document's subject, period, terminology or names to help "
        "resolve uncertain readings. Leave the description empty to infer the subject from earlier recognized text. "
        "Context is used when you click Ask AI for readings in word or passage review."), &dialog);
    explanation->setWordWrap(true);
    layout->addWidget(explanation);
    auto *description = new QPlainTextEdit(document.context.description, &dialog);
    description->setObjectName(QStringLiteral("documentDescription"));
    description->setPlaceholderText(tr("For example: A German physics textbook about thermodynamics, published in 1920."));
    layout->addWidget(description);
    auto *infer = new QCheckBox(tr("Use earlier recognized text to infer subject and terminology"), &dialog);
    infer->setObjectName(QStringLiteral("inferDocumentContext"));
    infer->setChecked(document.context.inferFromRecognizedText);
    layout->addWidget(infer);
    auto *disclosure = new QLabel(tr("AI review sends the description and selected reliable excerpts along with "
        "the crop and nearby text to your configured model. Suggestions require your review against the scan."), &dialog);
    disclosure->setWordWrap(true);
    layout->addWidget(disclosure);
    auto *length = new QLabel(&dialog);
    layout->addWidget(length);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    const auto validate = [&] {
        const int count = description->toPlainText().size();
        length->setText(tr("%1 / %2 characters").arg(count).arg(core::DocumentContext::maximumDescriptionLength));
        buttons->button(QDialogButtonBox::Ok)->setEnabled(count <= core::DocumentContext::maximumDescriptionLength);
    };
    connect(description, &QPlainTextEdit::textChanged, &dialog, validate);
    validate();
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    core::DocumentContext context{description->toPlainText().trimmed(), infer->isChecked()};
    if (context.description.size() > core::DocumentContext::maximumDescriptionLength || context == document.context) return;
    undoStack.push(new ChangeDocumentContextCommand(document, std::move(context)));
}

void MainWindow::reviewNextSpecialist()
{
    if (processor.isBusy() || exportBusy || !maybeApplyEditor()) return;
    const auto *page = document.page(currentPage);
    if (!page || page->words.isEmpty()) return;
    const int count = wordList->count();
    if (count == 0) return;
    int index = selectedWords.isEmpty() ? 0 : (selectedWords.last() + 1) % count;
    for (int n = 0; n < count; ++n, index = (index + 1) % count) {
        if (page->specialistSpan(index).isEmpty()) continue;
        selectWord(index);
        reviewSelectedWord();
        return;
    }
    statusBar()->showMessage(tr("No specialist readings to review on this page."), 5000);
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
        !processing::isPagedSource(originalPath) || !maybeApplyEditor()) return;
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
    formattedPdfAction->setEnabled(!processor.isBusy() && !exportBusy && document.wordCount() > 0);
    nextSpecialistAction->setEnabled(!processor.isBusy() && !exportBusy && !imagePath.isEmpty() &&
        page && !page->words.isEmpty());
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
        const int i = wordList->count();
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
        if (!page->specialistSpan(i).isEmpty()) {
            item->setIcon(style()->standardIcon(QStyle::SP_FileDialogContentsView));
            item->setToolTip(item->toolTip() + tr("\nReview an alternative from %1").arg(word.alternative.engine));
        }
    }
    QImageReader regionPreviewReader(imagePath);
    regionPreviewReader.setAutoTransform(true);
    const QImage regionPreview = page->imageRegions.isEmpty() ? QImage() : regionPreviewReader.read();
    for (int i = 0; i < page->imageRegions.size(); ++i) {
        const ImageRegion &region = page->imageRegions.at(i);
        auto *box = scene->addRect(region.bounds, QPen(QColor(25, 135, 55), 2),
                                   QBrush(QColor(35, 160, 70, 35)));
        box->setData(1, i);
        box->setData(2, region.notText);
        box->setZValue(4);
        box->setAcceptedMouseButtons(Qt::NoButton);
        box->setToolTip(tr("Region %1 · drag to move; drag an edge to resize").arg(i + 1));
        imageRegionBoxes.append(box);
        const QString language = region.notText ? tr("Not a text") :
            region.language.isEmpty() ? tr("Page language") : region.language;
        auto *item = new QListWidgetItem(QIcon(QPixmap::fromImage(regionPreview.copy(region.bounds)
            .scaled(regionList->iconSize(), Qt::KeepAspectRatio, Qt::SmoothTransformation))),
            tr("Block %1 · %2\n%3 × %4 px").arg(i + 1).arg(language)
                .arg(region.bounds.width()).arg(region.bounds.height()), regionList);
        item->setSizeHint(regionList->gridSize());
        item->setTextAlignment(Qt::AlignHCenter);
        item->setToolTip(tr("Block %1 · %2\nRight-click to select its language or remove it.")
            .arg(i + 1).arg(language));
        if (region.notText) item->setForeground(QColor(110, 110, 110));
    }
    ocrButton->setText(tr("Recognize"));
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
    for (const auto &region : edited.imageRegions)
        if (area.intersects(region.bounds)) {
            statusBar()->showMessage(tr("Blocks cannot overlap. Draw in an empty part of the page."), 5000);
            return;
        }
    if (edited.imageRegions.size() >= 1000) { showError(tr("This page has too many regions.")); return; }
    if (edited.width == 0) {
        edited.id = QStringLiteral("page-%1").arg(currentPage);
        edited.number = currentPage;
        const QSize size = scene->sceneRect().size().toSize();
        edited.width = size.width();
        edited.height = size.height();
        edited.sourceWidth = rawImage.width();
        edited.sourceHeight = rawImage.height();
        edited.preprocessing = displayedPreprocessing;
    }
    edited.imageRegions.append({QUuid::createUuid().toString(QUuid::WithoutBraces), area});
    selectedImageRegion = static_cast<int>(edited.imageRegions.size()) - 1;
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
        const bool notText = page->imageRegions[i].notText;
        imageRegionBoxes[i]->setPen(QPen(active ? QColor(210, 65, 25) : notText ? QColor(110, 110, 110) : QColor(25, 135, 55),
                                         active ? 3 : 2, notText ? Qt::DashLine : Qt::SolidLine));
        imageRegionBoxes[i]->setBrush(active ? QColor(230, 90, 35, 45) : notText ? QColor(100, 100, 100, 20) : QColor(35, 160, 70, 35));
    }
    deleteRegionButton->setEnabled(selectedImageRegion >= 0 && !processor.isBusy() && !exportBusy);
    regionOcrButton->setEnabled(selectedImageRegion >= 0 && !page->imageRegions[selectedImageRegion].notText &&
                                !processor.isBusy() && !exportBusy);
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
    for (int i = 0; i < page->imageRegions.size(); ++i)
        if (i != index && area.intersects(page->imageRegions[i].bounds)) { showDocument(); return; }
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

QString MainWindow::blockLanguage(const ImageRegion &region) const
{
    return region.language.isEmpty() ? languageEdit->text().trimmed() : region.language;
}

void MainWindow::setBlockLanguage(int index, const QString &language, bool notText)
{
    const auto *page = document.page(currentPage);
    if (!page || index < 0 || index >= page->imageRegions.size() || processor.isBusy() ||
        exportBusy || !maybeApplyEditor()) return;
    OcrPage edited = *document.page(currentPage);
    auto &region = edited.imageRegions[index];
    if (region.language == language && region.notText == notText) return;
    region.language = language;
    region.notText = notText;
    if (notText) edited.words.removeIf([&](const OcrWord &word) { return overlapsWord(word, region.bounds); });
    selectedImageRegion = index;
    undoStack.push(new ReplacePageCommand(document, currentPage, std::move(edited), tr("Select block language")));
}

void MainWindow::showBlockContextMenu(int index, const QPoint &globalPosition)
{
    if (processor.isBusy() || exportBusy || !maybeApplyEditor()) return;
    activateImageRegion(index);
    const auto *page = document.page(currentPage);
    if (!page || index < 0 || index >= page->imageRegions.size()) return;
    const auto region = page->imageRegions[index];
    QMenu menu(this);
    menu.setObjectName(QStringLiteral("blockContextMenu"));
    auto *recognize = menu.addAction(recognizeActionIcon(), tr("Recognize block"));
    recognize->setIconVisibleInMenu(true);
    recognize->setEnabled(!region.notText);
    connect(recognize, &QAction::triggered, this, &MainWindow::runRegionOcr);
    auto *languages = menu.addMenu(tr("Select block language"));
    const auto addLanguage = [&](const QString &label, const QString &code, bool notText = false) {
        auto *action = languages->addAction(label);
        action->setData(code);
        action->setCheckable(true);
        action->setChecked(region.notText == notText && (notText || region.language == code));
        connect(action, &QAction::triggered, this, [this, index, code, notText] { setBlockLanguage(index, code, notText); });
    };
    addLanguage(tr("Use page language (%1)").arg(languageEdit->text()), {});
    addLanguage(tr("Not a text"), region.language, true);
    languages->addSeparator();
    const bool tesseract = processor.engineId() == QStringLiteral("tesseract");
    const QStringList labels{tr("Automatic"), tr("English"), tr("German"), tr("French"), tr("Spanish"),
        tr("Italian"), tr("Portuguese"), tr("Russian"), tr("Ukrainian"), tr("Chinese"), tr("Japanese"),
        tr("Korean"), tr("Arabic"), tr("Hebrew")};
    const QStringList codes = tesseract
        ? QStringList{"", "eng", "deu", "fra", "spa", "ita", "por", "rus", "ukr", "chi_sim", "jpn", "kor", "ara", "heb"}
        : QStringList{"auto", "en", "de", "fr", "es", "it", "pt", "ru", "uk", "zh", "ja", "ko", "ar", "he"};
    for (int i = tesseract ? 1 : 0; i < labels.size(); ++i) addLanguage(labels[i], codes[i]);
    auto *other = languages->addAction(tr("Other language…"));
    connect(other, &QAction::triggered, this, [this, index, region] {
        bool ok = false;
        const QString code = QInputDialog::getText(this, tr("Select block language"),
            tr("OCR language code or combination, for example eng+deu:"), QLineEdit::Normal, region.language, &ok).trimmed();
        if (ok && QRegularExpression(QStringLiteral("^[A-Za-z0-9_+/-]{1,64}$")).match(code).hasMatch())
            setBlockLanguage(index, code, false);
    });
    auto *remove = menu.addAction(removeBlockIcon(), tr("Remove block"));
    remove->setIconVisibleInMenu(true);
    connect(remove, &QAction::triggered, this, [this, index] { deleteImageRegion(index); });
    menu.exec(globalPosition);
}

void MainWindow::refreshSpellLanguage()
{
    if (!spellHighlighter || !spellLanguage) return;
    QStringList languages;
    const QString chosen = spellLanguage->currentData().toString();
    if (!chosen.isEmpty()) languages.append(chosen);
    else {
        const auto *page = document.page(currentPage);
        const QString fallback = page && !page->language.isEmpty() && page->language != "auto"
            ? page->language : languageEdit->text().trimmed();
        if (page && selectedImageRegion >= 0 && selectedImageRegion < page->imageRegions.size()) {
            const auto &region = page->imageRegions[selectedImageRegion];
            languages.append(blockLanguage(region));
        } else if (page && !selectedWords.isEmpty()) {
            for (int index : selectedWords) {
                if (index < 0 || index >= page->words.size()) continue;
                QString code = fallback;
                for (const auto &region : page->imageRegions)
                    if (!region.notText && region.bounds.contains(page->words[index].bounds.center())) {
                        code = blockLanguage(region);
                        break;
                    }
                if (!languages.contains(code)) languages.append(code);
            }
        } else languages.append(fallback);
    }
    const QSignalBlocker blocker(selectionEditor);
    spellHighlighter->setLanguages(languages);
    spellCheckAction->setToolTip(spellHighlighter->statusText());
    spellLanguage->setToolTip(spellHighlighter->statusText());
}

void MainWindow::refreshReviewLabel()
{
    QString reviewLabel = selectedWords.size() > 1 ? tr("Review passage…") : tr("Review word…");
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
    if (selectedWords.size() == 1) {
        const auto *page = document.page(currentPage);
        if (page && !page->specialistSpan(selectedWords.first()).isEmpty()) reviewLabel = tr("Review specialist reading…");
    }
    reviewWordButton->setText(reviewLabel);
}

void MainWindow::updateSelection(const QVector<int> &indices)
{
    selectedWords = indices;
    refreshReviewLabel();
    reviewWordButton->setEnabled(!processor.isBusy() && !exportBusy && !imagePath.isEmpty() && canReviewSelection());
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
    const bool multipleBlocks = indices.size() > 1 && std::ranges::any_of(indices,
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
        if (!word->fontFamily.isEmpty()) format.setFontFamilies({word->fontFamily});
        format.setFontWeight(word->bold ? QFont::Bold : QFont::Normal);
        format.setFontItalic(word->italic);
        if (word->fontSizePt > 0) format.setFontPointSize(word->fontSizePt);
        cursor.insertText(word->text, format);
        previous = word;
    }
    selectionEditor->moveCursor(QTextCursor::Start);
    refreshSpellLanguage();
}

bool MainWindow::canReviewSelection() const
{
    if (selectedWords.isEmpty()) return false;
    const auto *page = document.page(currentPage);
    if (!page) return false;
    for (int i = 0; i < selectedWords.size(); ++i) {
        const int index = selectedWords[i];
        if (index < 0 || index >= page->words.size() || (i && index != selectedWords[i - 1] + 1) ||
            (selectedWords.size() > 1 && page->words[index].layoutBlock)) return false;
    }
    return true;
}

void MainWindow::reviewSelectedWord()
{
    if (processor.isBusy() || exportBusy || imagePath.isEmpty() || !canReviewSelection() ||
        !maybeApplyEditor() || !canReviewSelection()) return;
    const OcrPage *page = document.page(currentPage);
    if (!page || selectedWords.first() < 0 || selectedWords.first() >= page->words.size()) return;
    const int index = selectedWords.first();
    const OcrWord &word = page->words.at(index);
    auto span = selectedWords.size() == 1 ? page->specialistSpan(index) : QVector<int>{};
    const bool specialist = !span.isEmpty();
    if (!specialist) span = selectedWords;
    const int last = span.last();
    QImageReader reader(imagePath);
    reader.setAutoTransform(true);
    const QImage image = reader.read();
    if (image.isNull()) { showError(tr("Could not load the page image for review.")); return; }
    QRect bounds = word.bounds;
    for (int i : span) bounds |= page->words[i].bounds;
    bounds = bounds.intersected(image.rect());
    if (bounds.isEmpty()) { showError(tr("This reading has no usable source image box.")); return; }
    const QRect cropRect = bounds.adjusted(-qMax(45, bounds.width() / 2),
        -qMax(25, bounds.height()), qMax(45, bounds.width() / 2),
        qMax(25, bounds.height())).intersected(image.rect());
    QStringList context;
    for (int i = qMax(0, index - 8); i < index; ++i) context.append(page->words[i].text);
    context.append(QStringLiteral("[…]"));
    for (int i = last + 1; i < page->words.size() && i <= last + 8; ++i) context.append(page->words[i].text);
    QStringList primary;
    for (int i : span) primary.append(page->words[i].text);
    const QString baseline = primary.join(' ');
    AiReviewDialog dialog(baseline, context.join(' '), image.copy(cropRect),
                          bounds.translated(-cropRect.topLeft()), aiReviewToken, this,
                          specialist ? &word.alternative : nullptr, span.size() > 1 || word.layoutBlock);
    dialog.setDocumentContext(core::reconstructionContext(document, currentPage, index));
    if (!specialist && span.size() == 1) {
        if (!correctionMemory) correctionMemory.emplace(document, similarSymbolsAction->isChecked());
        dialog.setBookSuggestions(correctionMemory->suggestions(*page, index));
    }
    const int result = dialog.exec();
    aiReviewToken = dialog.sessionToken();
    if (result != QDialog::Accepted) return;
    if (specialist || span.size() > 1) {
        auto edited = specialist ? core::WordEditor::acceptSpecialist(*page, index, dialog.correction())
                                 : core::WordEditor::acceptPassage(*page, span, dialog.correction());
        if (!edited) { showError(edited.error()); return; }
        undoStack.push(new ReplacePageCommand(document, currentPage, std::move(*edited), tr("Review passage")));
        return;
    }
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
        if (block.text != plain) block.readingEngine.clear();
        block.alternative = {};
        block.text = plain;
        block.reviewed = true;
        QTextCursor cursor(selectionEditor->document());
        cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
        block.bold = cursor.charFormat().fontWeight() >= QFont::Bold;
        block.italic = cursor.charFormat().fontItalic();
        block.fontFamily = cursor.charFormat().fontFamilies().toStringList().value(0);
        if (cursor.charFormat().fontPointSize() > 0) block.fontSizePt = qRound(cursor.charFormat().fontPointSize());
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
        cursor.setPosition(static_cast<int>(match.capturedStart()));
        cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
        const QTextCharFormat format = cursor.charFormat();
        tokens.append({.text = match.captured(),
                       .bold = format.fontWeight() >= QFont::Bold,
                       .italic = format.fontItalic(),
                       .fontSizePt = format.fontPointSize() > 0 ? qRound(format.fontPointSize()) : 0,
                       .fontFamily = format.fontFamilies().toStringList().value(0)});
    }
    const OcrPage *page = document.page(currentPage);
    if (!page) return false;
    auto edited = core::WordEditor::apply(*page, selectedWords, tokens);
    if (!edited) { showError(edited.error()); return false; }
    const qsizetype originalCount = selectedWords.size();
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
    const auto *page = document.page(currentPage);
    const auto *block = page && selectedImageRegion >= 0 && selectedImageRegion < page->imageRegions.size()
        ? &page->imageRegions[selectedImageRegion] : nullptr;
    if (block && block->notText) return;
    bool containsCorrections = false;
    for (int index : selectedWords)
        containsCorrections = containsCorrections || displayedWords.at(index)->reviewed;
    if (containsCorrections && QMessageBox::question(this, tr("Recognize region again"),
        tr("This region contains corrected words. New OCR may replace unmatched corrections. Continue?"))
        != QMessageBox::Yes) return;
    status->setText(tr("Recognizing selected region…"));
    processor.recognizeRegion(imagePath, block ? blockLanguage(*block) : languageEdit->text().trimmed(), currentPage, selectedRegion);
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

void MainWindow::convertDjVuToPdf()
{
    if (processor.isBusy() || exportBusy || imagePath.isEmpty() ||
        processing::pageSourceKind(originalPath) != processing::PageSourceKind::DjVu || !maybeApplyEditor()) return;
    chooseExportPath(tr("Convert DjVu to PDF"), tr("PDF files (*.pdf)"), QStringLiteral("pdf"), ExportFormat::Pdf);
}

void MainWindow::exportPdf(bool archive, bool formatted)
{
    if (processor.isBusy() || exportBusy || document.wordCount() == 0 || !maybeApplyEditor()) return;
    if (document.pages.size() < document.totalPages &&
        QMessageBox::question(this, tr("Pages without OCR"),
            archive ? tr("Some pages have not been recognized in YAGF. OCRmyPDF may recognize them during PDF/A conversion without YAGF corrections. Export anyway?")
                    : tr("Some pages have not been recognized and will have no searchable text. Export anyway?"),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) return;
    chooseExportPath(archive ? tr("Export archival PDF/A") : formatted ? tr("Export formatted PDF") : tr("Export searchable PDF"),
        tr("PDF files (*.pdf)"), QStringLiteral("pdf"),
        archive ? ExportFormat::ArchivalPdf : formatted ? ExportFormat::FormattedPdf : ExportFormat::Pdf);
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
    const bool hasExamples = std::ranges::any_of(std::as_const(document.pages), [](const auto &page) {
        return std::ranges::any_of(page.words, core::isVerifiedCorrection);
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
        const auto progress = [this](qsizetype completed, qsizetype total) {
            emit exportProgress(completed, total);
        };
        if (format == ExportFormat::ScientificBook)
            return processing::ScientificBookExporter::write(snapshot, path, *cancel, progress);
        if (format == ExportFormat::Samples)
            return processing::VerifiedSampleExporter::write(snapshot, path, *cancel, progress);
        return processing::PdfExporter::write(snapshot, path, format == ExportFormat::ArchivalPdf, *cancel, progress,
            format == ExportFormat::FormattedPdf ? processing::PdfExporter::Mode::FormattedText :
                                                  processing::PdfExporter::Mode::SearchableScan);
    }));
}

} // namespace yagf::ui

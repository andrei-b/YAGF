#include "pdfexporter.h"
#include "detail/pdfformulas.h"
#include "formulaeditor.h"
#include "exportpage.h"
#include "scientificbookexporter.h"
#include "latexdocumentexporter.h"
#include "verifiedsampleexporter.h"
#include "mainwindow.h"
#include <QScopeGuard>
#include "blockminiaturelist.h"
#include "projectfile.h"
#include "preprocessdialog.h"
#include "restorationdialog.h"
#include <QBuffer>
#include "imageprocessor.h"
#include "formulagrowth.h"
#include "layoutdialog.h"
#include "aipagelayoutdialog.h"
#include "aipageicons.h"
#include "nativetrainingdialog.h"
#include "ocrreadings.h"
#include "ocrservicesdialog.h"
#include "ocrlanguagedialog.h"
#include "ocrreadingsdialog.h"
#include "scannerdialog.h"
#include "documentcommands.h"
#include "wordeditor.h"
#include "correctionretention.h"
#include "pagegeometry.h"
#include "remoteocrengine.h"
#include "tesseractengine.h"
#include "aireviewdialog.h"
#include "regiongeometry.h"
#include "spellhighlighter.h"
#include "verificationcontroller.h"
#include "pagesource.h"
#include "symbolrulesdialog.h"
#include "formulatext.h"
#include "blockfont.h"

#include <QCloseEvent>
#include <QApplication>
#include <QClipboard>
#include <QCheckBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QScrollBar>
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
#include <QGridLayout>
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
#include <QProgressDialog>
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
#include <QWidgetAction>
#include <QTimer>
#include <QCursor>
#include <QEventLoop>
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
class ProjectLoadingDialog final : public QProgressDialog {
public:
    using QProgressDialog::QProgressDialog;
    void reject() override {}
protected:
    void closeEvent(QCloseEvent *event) override { event->ignore(); }
};

class ExportSaveDialog : public QFileDialog {
public:
    using QFileDialog::QFileDialog;
    bool imageExport = false;
    void accept() override
    {
        if (imageExport) {
            const bool jpeg = selectedNameFilter() == nameFilters().last();
            setDefaultSuffix(jpeg ? "jpg" : "png");
            const QFileInfo file(selectedFiles().value(0));
            const auto ext = file.suffix().toLower();
            if (!file.isDir() && ext != "png" && ext != "jpg" && ext != "jpeg") {
                QMessageBox::information(this, tr("Export scanned page"), tr("Choose a PNG or JPEG filename."));
                return;
            }
            if (!file.isDir() && ((ext == "png") == jpeg))
                selectFile(file.dir().filePath(file.completeBaseName() + "." + defaultSuffix()));
        }
        QFileDialog::accept();
    }
};

void addToolbarSeparator(QToolBar *toolbar)
{
    auto *action = toolbar->addSeparator();
    // Keep the styled width while the toolbar settles its initial layout.
    toolbar->widgetForAction(action)->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Minimum);
}

QIcon recognizeActionIcon(bool allPages = false)
{
    QPixmap pixmap(32, 32);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor("#216ec1"), 1.8));
    painter.setBrush(QColor("#edf5ff"));
    if (allPages) {
        painter.drawRoundedRect(QRectF(11, 3, 17, 22), 2, 2);
        painter.drawRoundedRect(QRectF(7, 6, 17, 22), 2, 2);
    }
    painter.setBrush(Qt::white);
    painter.drawRoundedRect(QRectF(allPages ? 3 : 7, allPages ? 9 : 4, 17, 23), 2, 2);
    const int x = allPages ? 7 : 11, y = allPages ? 15 : 10;
    for (int row = 0; row < 3; ++row) painter.drawLine(x, y+4*row, x+9, y+4*row);
    return QIcon(pixmap);
}

QIcon removeBlockIcon()
{
    return QIcon::fromTheme(QStringLiteral("edit-delete"),
                           QApplication::style()->standardIcon(QStyle::SP_TrashIcon));
}

QIcon toolbarPinIcon()
{
    QPixmap pixmap(24, 24);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.translate(12, 11);
    painter.rotate(35);
    painter.setPen(QPen(QColor("#216ec1"), 1.8, Qt::SolidLine, Qt::RoundCap));
    painter.setBrush(QColor("#c9dff8"));
    painter.drawRoundedRect(QRectF(-5, -8, 10, 5), 1, 1);
    painter.drawLine(-3, -3, -3, 2);
    painter.drawLine(3, -3, 3, 2);
    painter.drawLine(-6, 2, 6, 2);
    painter.drawLine(0, 2, 0, 10);
    return QIcon(pixmap);
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

QString contentLabel(const QString &content)
{
    if (content == "picture") return QObject::tr("Picture");
    if (content == "text") return QObject::tr("Text");
    if (content == "math") return QObject::tr("Math");
    if (content == "mixed") return QObject::tr("Text + math");
    return QObject::tr("Use page engine");
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

void carryCorrections(OcrPage &fresh, const OcrPage *oldPage, bool estimateFormatting, bool keepLayout)
{
    if (!oldPage) return;
    auto detectedBlocks = std::move(fresh.imageRegions);
    core::retainCorrections(fresh, *oldPage, estimateFormatting);
    if (!detectedBlocks.isEmpty() || fresh.engineId == "local-mixed") fresh.imageRegions = std::move(detectedBlocks);
    if (!keepLayout) {
        fresh.imageRegions.removeIf([](const auto &block) { return block.autoDetected && block.detectedLabel.isEmpty(); });
        return;
    }
    const auto hasLayout = [](const QString &engine) {
        return engine == "surya-remote" || engine == "rapid-latex-ocr" || engine == "local-mixed";
    };
    const bool structured = hasLayout(fresh.engineId) || hasLayout(oldPage->engineId);
    const bool sameEngine = oldPage->engineId == fresh.engineId;
    QSet<int> matchingBlocks;
    for (const auto &word : fresh.words) for (const auto &old : oldPage->words)
        if ((word.bounds == old.bounds && word.originalText == old.originalText) ||
            (word.reviewed && old.reviewed && word.id == old.id && word.text == old.text))
            if (structured && sameEngine && word.block == old.block) matchingBlocks.insert(word.block);
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
    viewport()->setCursor(Qt::CrossCursor);
    imageToolbar = new QToolBar(viewport());
    imageToolbar->setObjectName(QStringLiteral("imageToolbar"));
    imageToolbar->setMovable(false);
    imageToolbar->setFloatable(false);
    imageToolbar->setIconSize(QSize(20, 20));
    imageToolbar->setCursor(Qt::ArrowCursor);
    auto *out = imageToolbar->addAction(QIcon(":/legacy/smaller.png"), tr("Zoom out"), this, [this] { zoomBy(1.0/1.25); });
    out->setObjectName("zoomOutAction");
    auto *fit = imageToolbar->addAction(style()->standardIcon(QStyle::SP_TitleBarMaxButton), tr("Fit page"), this, &PageView::fitPage);
    fit->setObjectName("fitPageAction");
    qobject_cast<QToolButton *>(imageToolbar->widgetForAction(fit))->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    auto *in = imageToolbar->addAction(QIcon(":/legacy/larger.png"), tr("Zoom in"), this, [this] { zoomBy(1.25); });
    in->setObjectName("zoomInAction");
    pinImageToolbar = imageToolbar->addAction(toolbarPinIcon(), tr("Pinned"));
    pinImageToolbar->setObjectName("pinImageToolbarAction");
    pinImageToolbar->setCheckable(true);
    pinImageToolbar->setChecked(QSettings("YAGF", "YAGF Revival").value("view/imageToolbarPinned", true).toBool());
    pinImageToolbar->setToolTip(tr("Pin the image toolbar. When unpinned, move the pointer to the top of the image to show it."));
    qobject_cast<QToolButton *>(imageToolbar->widgetForAction(pinImageToolbar))->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    imageToolbar->insertAction(out, pinImageToolbar);
    imageToolbar->insertSeparator(out);
    hideImageToolbar = new QTimer(this);
    hideImageToolbar->setSingleShot(true);
    hideImageToolbar->setInterval(200);
    connect(hideImageToolbar, &QTimer::timeout, this, [this] { updateImageToolbar(viewport()->mapFromGlobal(QCursor::pos())); });
    connect(pinImageToolbar, &QAction::toggled, this, [this](bool pinned) {
        QSettings("YAGF", "YAGF Revival").setValue("view/imageToolbarPinned", pinned);
        pinImageToolbar->setText(pinned ? tr("Pinned") : tr("Pin"));
        positionImageToolbar();
        updateImageToolbar(viewport()->mapFromGlobal(QCursor::pos()));
    });
    pinImageToolbar->setText(pinImageToolbar->isChecked() ? tr("Pinned") : tr("Pin"));
    imageToolbar->installEventFilter(this);
    for (auto *child : imageToolbar->findChildren<QWidget *>()) { child->setCursor(Qt::ArrowCursor); child->installEventFilter(this); }
    positionImageToolbar();
    imageToolbar->setVisible(pinImageToolbar->isChecked());
}

void PageView::positionImageToolbar()
{
    if (!imageToolbar) return;
    imageToolbar->adjustSize();
    imageToolbar->resize(std::min(imageToolbar->sizeHint().width(), std::max(1, viewport()->width()-16)), imageToolbar->sizeHint().height());
    imageToolbar->move(std::max(0, (viewport()->width()-imageToolbar->width())/2), 10);
    imageToolbar->raise();
}

void PageView::updateImageToolbar(const QPoint &position)
{
    if (!imageToolbar) return;
    const bool nearTop = viewport()->rect().contains(position) && position.y() <= std::max(56, imageToolbar->height()+24);
    imageToolbar->setVisible(pinImageToolbar->isChecked() || nearTop);
    if (imageToolbar->isVisible()) imageToolbar->raise();
}

bool PageView::viewportEvent(QEvent *event)
{
    if (imageToolbar) {
        if (event->type() == QEvent::MouseMove)
            updateImageToolbar(static_cast<QMouseEvent *>(event)->position().toPoint());
        else if (event->type() == QEvent::Leave) hideImageToolbar->start();
        else if (event->type() == QEvent::Enter) updateImageToolbar(viewport()->mapFromGlobal(QCursor::pos()));
        else if (event->type() == QEvent::Resize) positionImageToolbar();
    }
    return QGraphicsView::viewportEvent(event);
}

bool PageView::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::Enter || event->type() == QEvent::HoverEnter) {
        hideImageToolbar->stop();
        if (auto *widget = qobject_cast<QWidget *>(watched)) widget->setCursor(Qt::ArrowCursor);
        viewport()->setCursor(Qt::ArrowCursor);
    }
    else if (event->type() == QEvent::Leave) hideImageToolbar->start();
    else if (watched == imageToolbar && event->type() == QEvent::LayoutRequest) {
        positionImageToolbar();
        for (auto *child : imageToolbar->findChildren<QWidget *>()) { child->setCursor(Qt::ArrowCursor); child->installEventFilter(this); }
    }
    return QGraphicsView::eventFilter(watched, event);
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
    if (event->button() == Qt::MiddleButton) {
        panPosition = event->pos();
        viewport()->setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton &&
        event->modifiers().testFlag(Qt::ControlModifier)) {
        for (QGraphicsItem *hit : items(event->pos()))
            if (auto *box = qgraphicsitem_cast<QGraphicsPathItem *>(hit);
                box && box->data(0).isValid()) {
                emit wordClicked(box->data(0).toInt());
                event->accept();
                return;
            }
    }
    if (event->button() == Qt::LeftButton) {
        setFocus();
        if (auto *item = qgraphicsitem_cast<QGraphicsRectItem *>(itemAt(event->pos()));
            item && item->data(1).isValid()) {
            const int index = item->data(1).toInt();
            const bool toggle = event->modifiers().testFlag(Qt::ShiftModifier) ||
                                event->modifiers().testFlag(Qt::ControlModifier);
            emit regionActivated(index, toggle);
            if (toggle || activeRegionIndex != index) { event->accept(); return; }
            item = qgraphicsitem_cast<QGraphicsRectItem *>(itemAt(event->pos()));
            if (!item || !item->data(1).isValid() || item->data(1).toInt() != index) {
                event->accept();
                return;
            }
            editingRegionIndex = index;
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
    if (panPosition) {
        const QPoint delta = event->pos() - *panPosition;
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - delta.x());
        verticalScrollBar()->setValue(verticalScrollBar()->value() - delta.y());
        panPosition = event->pos();
        event->accept();
        return;
    }
    if (editingRegionItem || rubberBand->isVisible()) {
        updateRegionDrag(event->pos());
        event->accept();
        return;
    }
    if (event->buttons() == Qt::NoButton) {
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
    if (event->button() == Qt::MiddleButton && panPosition) {
        panPosition.reset();
        viewport()->setCursor(Qt::CrossCursor);
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton && editingRegionItem) {
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
    if (event->button() == Qt::LeftButton && rubberBand->isVisible()) {
        updateRegionDrag(event->pos());
        rubberBand->hide();
        if (drawingRegion.width() >= 8 && drawingRegion.height() >= 8)
            emit regionSelected(QRectF(drawingRegion));
        else if (drawingRegion.width() < 8 && drawingRegion.height() < 8)
            emit regionActivated(-1);
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
    } else {
        emit pageContextMenuRequested(event->globalPos());
        event->accept();
    }
}

void PageView::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape && !editingRegionItem && !rubberBand->isVisible()) {
        emit regionActivated(-1);
        event->accept();
        return;
    }
    if (activeRegionIndex >= 0 &&
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

void PageView::scrollContentsBy(int dx, int dy)
{
    QGraphicsView::scrollContentsBy(dx, dy);
    // Scrolling the viewport also moves its child widgets. Keep the controls
    // anchored to the visible window, independently of the scanned page.
    positionImageToolbar();
    updateImageToolbar(viewport()->mapFromGlobal(QCursor::pos()));
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
    toolsMenu->setObjectName("toolsMenu");
    ocrServicesAction=toolsMenu->addAction(tr("Recognition settings…"),this,&MainWindow::configureOcrServices);
    ocrServicesAction->setObjectName("ocrServicesAction");
    toolsMenu->addSeparator();
    auto *recognitionToolsMenu = toolsMenu->addMenu(tr("&Recognition"));
    recognitionToolsMenu->setObjectName("recognitionToolsMenu");
    auto *pageToolsMenu = toolsMenu->addMenu(tr("&Page && layout"));
    pageToolsMenu->setObjectName("pageToolsMenu");
    auto *reviewToolsMenu = toolsMenu->addMenu(tr("Re&view"));
    reviewToolsMenu->setObjectName("reviewToolsMenu");
    auto *trainingToolsMenu = toolsMenu->addMenu(tr("&Training && experiments"));
    trainingToolsMenu->setObjectName("trainingToolsMenu");
    aiPageAreasAction = pageToolsMenu->addAction(aiPageAreasIcon(), tr("AI page areas…"), this, &MainWindow::classifyPageAreas);
    aiPageAreasAction->setObjectName("aiPageAreasAction");
    aiPageAreasAction->setIconVisibleInMenu(true);
    aiPageAreasAction->setEnabled(false);
    aiSplitBlockAction=pageToolsMenu->addAction(aiPageAreasIcon(),tr("AI split selected block…"),this,&MainWindow::splitSelectedBlockWithAi);
    aiSplitBlockAction->setObjectName("aiSplitBlockAction");aiSplitBlockAction->setEnabled(false);
    refineFormulaAction = pageToolsMenu->addAction(tr("Refine formula blocks"), this, &MainWindow::refineFormulaBlocks);
    refineFormulaAction->setObjectName("refineFormulaBlocksAction");
    refineFormulaAction->setEnabled(false);
    verification = new VerificationController(this, document);
    auto *queueAction = reviewToolsMenu->addAction(tr("Verification queue"), verification, &VerificationController::show);
    queueAction->setObjectName(QStringLiteral("showVerificationQueue"));
    auto *nextIssue = reviewToolsMenu->addAction(tr("Next verification issue"), this, [this] { verification->navigate(1); });
    nextIssue->setShortcut(QKeySequence(Qt::Key_F4));
    auto *previousIssue = reviewToolsMenu->addAction(tr("Previous verification issue"), this, [this] { verification->navigate(-1); });
    previousIssue->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F4));
    reviewToolsMenu->addSeparator();
    connect(verification, &VerificationController::activateRequested, this, [this](const auto &item) { activateVerification(item); });
    connect(verification, &VerificationController::reviewRequested, this, [this](const auto &item) { activateVerification(item, 1); });
    connect(verification, &VerificationController::verifyRequested, this, [this](const auto &item) { activateVerification(item, 2); });
    connect(verification, &VerificationController::discardRequested, this, [this](const auto &item) { activateVerification(item, 3); });
    auto *experimentsAction = trainingToolsMenu->addAction(tr("OCR experiments…"),this,&MainWindow::openExperiments);
    experimentsAction->setObjectName(QStringLiteral("ocrExperiments"));
    renderingAction = pageToolsMenu->addAction(tr("Rendering resolution…"), this, &MainWindow::configureRendering);
    renderingAction->setObjectName(QStringLiteral("renderingResolution"));
    pageToolsMenu->addSeparator();

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
    scanAction = fileMenu->addAction(QIcon::fromTheme("scanner", style()->standardIcon(QStyle::SP_ComputerIcon)),
                                     tr("Scan a page…"), this, &MainWindow::scanPage);
    scanAction->setIconText(tr("Scan"));
    toolbar->addAction(scanAction);
    auto *pasteAction = fileMenu->addAction(tr("Paste image from clipboard"), this, &MainWindow::pasteImage);
    pasteAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+V")));
    fileMenu->addAction(tr("Open project"), this, &MainWindow::openProject);
    fileMenu->addAction(tr("Open portable project folder…"), this, &MainWindow::openPortableProject);
    recentProjectsMenu = fileMenu->addMenu(tr("Recent projects"));
    recentProjectsMenu->setObjectName(QStringLiteral("recentProjectsMenu"));
    connect(fileMenu, &QMenu::aboutToShow, this, &MainWindow::refreshRecentProjects);
    refreshRecentProjects();
    fileMenu->addAction(tr("Save project"), this, &MainWindow::saveProject);
    auto *saveAction = toolbar->addAction(style()->standardIcon(QStyle::SP_DialogSaveButton),
                                          tr("Save"), this, &MainWindow::saveProject);
    saveAction->setToolTip(tr("Save the current project"));
    fileMenu->addAction(tr("Save project as…"), this, &MainWindow::saveProjectAs);
    fileMenu->addAction(tr("Save portable project folder…"), this, &MainWindow::savePortableProject);
    addToolbarSeparator(toolbar);
    previousButton = new QPushButton(QIcon(QStringLiteral(":/legacy/back.png")), {}, this);
    previousButton->setObjectName(QStringLiteral("pageNavButton"));
    previousButton->setToolTip(tr("Previous page (PgUp)"));
    nextButton = new QPushButton(QIcon(QStringLiteral(":/legacy/forward.png")), {}, this);
    nextButton->setObjectName(QStringLiteral("pageNavButton"));
    nextButton->setToolTip(tr("Next page (PgDown)"));
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
    addToolbarSeparator(toolbar);
    improveAction = toolbar->addAction(tr("Improve page…"), this, &MainWindow::improveCurrentPage);
    improveAction->setObjectName(QStringLiteral("improvePageAction"));
    pageToolsMenu->addAction(improveAction);
    restoreAction = pageToolsMenu->addAction(tr("Restore damaged page…"), this, &MainWindow::restoreCurrentPage);
    restoreAction->setObjectName(QStringLiteral("restorePageAction"));
    improveAction->setEnabled(false);
    restoreAction->setEnabled(false);
    pageToolsMenu->addSeparator();
    layoutAction = pageToolsMenu->addAction(tr("Edit layout…"), this, &MainWindow::editLayout);
    createRegionAction = pageToolsMenu->addAction(tr("Create region from selection"), this,
                                             &MainWindow::createRegionFromSelection);
    remainingAction = recognitionToolsMenu->addAction(tr("Recognize remaining pages"), this, [this] { runBatchOcr(true); });
    batchAction = recognitionToolsMenu->addAction(tr("Recognize all pages"), this, [this] { runBatchOcr(false); });
    batchAction->setObjectName(QStringLiteral("recognizeAllAction"));
    batchAction->setIcon(recognizeActionIcon(true));
    batchAction->setIconText(tr("Recognize all"));
    batchAction->setToolTip(tr("Recognize every page in the open document, including pages already recognized."));
    recognitionToolsMenu->addSeparator();
    rawOcrOutputAction = recognitionToolsMenu->addAction(tr("Use raw OCR output"));
    rawOcrOutputAction->setObjectName(QStringLiteral("rawOcrOutputAction"));
    rawOcrOutputAction->setCheckable(true);
    rawOcrOutputAction->setToolTip(tr("Use Tesseract's raw text and reading order without automatic block-layout correction or formula recognition. "
        "Leave unchecked for corrected layout."));
    connect(rawOcrOutputAction, &QAction::toggled, this, [this](bool enabled) {
        document.useRawOcrOutput = enabled;
        if (!originalPath.isEmpty()) { externalDirty = true; refreshDirty(); }
    });
    preserveFormattingAction = recognitionToolsMenu->addAction(tr("Preserve formatting"));
    preserveFormattingAction->setObjectName(QStringLiteral("preserveFormattingAction"));
    preserveFormattingAction->setCheckable(true);
    preserveFormattingAction->setToolTip(tr("Estimate the closest installed font, bold, italic and size during recognition. "
        "Latin printed text is supported; paragraph OCR blocks receive one estimated style."));
    connect(preserveFormattingAction, &QAction::toggled, this, [this](bool enabled) {
        document.preserveFormatting = enabled;
        if (!originalPath.isEmpty()) { externalDirty = true; refreshDirty(); }
    });
    similarSymbolsAction = trainingToolsMenu->addAction(tr("Match similar symbol images"));
    similarSymbolsAction->setCheckable(true);
    symbolRulesAction = trainingToolsMenu->addAction(tr("Symbol recognition rules…"));
    symbolRulesAction->setObjectName("symbolRulesAction");
    connect(symbolRulesAction, &QAction::triggered, this, [this] { editSymbolRules(false); });
    teachSymbolAction = trainingToolsMenu->addAction(tr("Teach selected symbol…"));
    teachSymbolAction->setObjectName("teachSelectedSymbolAction");
    teachSymbolAction->setEnabled(false);
    connect(teachSymbolAction, &QAction::triggered, this, [this] { editSymbolRules(true); });
    const QString similarSymbolsKey = QStringLiteral("review/matchSimilarSymbolImages");
    QSettings settings(QStringLiteral("YAGF"), QStringLiteral("YAGF Revival"));
    similarSymbolsAction->setChecked(settings.value(similarSymbolsKey, false).toBool());
    connect(similarSymbolsAction, &QAction::toggled, this, [this, similarSymbolsKey](bool enabled) {
        QSettings(QStringLiteral("YAGF"), QStringLiteral("YAGF Revival")).setValue(similarSymbolsKey, enabled);
        correctionMemory.reset();
        refreshReviewLabel();
    });
    trainingToolsMenu->addSeparator();
    nativeTrainingAction = trainingToolsMenu->addAction(aiPageAreasIcon(), tr("OCR training and adaptation…"), this, &MainWindow::trainNativeOcr);
    nativeTrainingAction->setObjectName("nativeTrainingAction");
    recognitionToolsMenu->addSeparator();
    recognitionToolsMenu->addAction(tr("OCR engine…"), this, &MainWindow::configureOcrEngine);
    languagePickerAction = recognitionToolsMenu->addAction(tr("Choose OCR languages…"), this,
                                                &MainWindow::chooseOcrLanguages);
    recognitionToolsMenu->addAction(tr("Specialist OCR…"), this, &MainWindow::configureSpecialists);
    ocrReadingsAction=reviewToolsMenu->addAction(tr("Review OCR readings…"),this,&MainWindow::reviewOcrReadings);
    ocrReadingsAction->setObjectName("ocrReadingsAction");
    documentContextAction = recognitionToolsMenu->addAction(tr("Document context…"), this, &MainWindow::configureDocumentContext);
    documentContextAction->setObjectName(QStringLiteral("documentContextAction"));
    documentContextAction->setEnabled(false);
    nextSpecialistAction = reviewToolsMenu->addAction(tr("Review next specialist reading…"), this, &MainWindow::reviewNextSpecialist);
    layoutAction->setEnabled(false);
    createRegionAction->setEnabled(false);
    batchAction->setEnabled(false);
    recognizeAction = new QAction(recognizeActionIcon(), tr("Recognize"), this);
    recognizeAction->setObjectName("recognizePageAction");
    recognizeAction->setEnabled(false);
    connect(recognizeAction, &QAction::triggered, this, &MainWindow::runOcr);
    ocrButton = new QToolButton(this);
    ocrButton->setDefaultAction(recognizeAction);
    ocrButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    ocrButton->setPopupMode(QToolButton::MenuButtonPopup);
    ocrButton->setIconSize(QSize(20, 20));
    ocrButton->setObjectName(QStringLiteral("primaryButton"));
    toolbar->addWidget(ocrButton);
    auto *recognitionMenu = new QMenu(ocrButton);
    recognitionMenu->setObjectName("recognitionMenu");
    recognitionMenu->addAction(recognizeAction);
    recognitionMenu->addAction(batchAction);
    recognitionMenu->addSeparator();
    recognitionMenu->addAction(ocrReadingsAction);
    mathMode = recognitionMenu->addAction(tr("Math only"));
    mathMode->setCheckable(true);
    mathMode->setObjectName(QStringLiteral("mathOcrMode"));
    mathMode->setToolTip(tr("Use the formula engine for every row. Leave unchecked for automatic text and math recognition with local Tesseract."));
    ocrButton->setMenu(recognitionMenu);
    recognitionMenu->addSeparator();
    unicodeFormulasAction=recognitionMenu->addAction(tr("Use Unicode formulas"));
    unicodeFormulasAction->setObjectName("unicodeFormulaOutputAction");
    unicodeFormulasAction->setCheckable(true);
    unicodeFormulasAction->setChecked(settings.value("ocr/unicodeFormulas",false).toBool());
    unicodeFormulasAction->setToolTip(tr("Write formulas as Unicode text, such as ψ and p₃. Original LaTeX is retained for editing and document export; unsupported expressions keep their LaTeX."));
    connect(unicodeFormulasAction,&QAction::toggled,this,[this](bool enabled) {
        if (!maybeApplyEditor()) { const QSignalBlocker blocker(unicodeFormulasAction); unicodeFormulasAction->setChecked(document.unicodeFormulas); return; }
        QSettings("YAGF","YAGF Revival").setValue("ocr/unicodeFormulas",enabled);
        if (document.unicodeFormulas==enabled) return;
        if (originalPath.isEmpty()) document.unicodeFormulas=enabled;
        else undoStack.push(new ChangeFormulaOutputCommand(document,enabled));
        int unsupported=0;
        if (enabled) for (const auto &page:document.pages) for (const auto &word:page.words)
            unsupported+=core::unicodeFormulaText(core::formulaSourceText(word),core::isFormulaWord(page,word)).unsupported;
        if (unsupported) statusBar()->showMessage(tr("%1 formula(s) kept in LaTeX: Unicode conversion is unsupported.").arg(unsupported),10000);
    });
    connect(mathMode, &QAction::toggled, this, [this](bool enabled) {
        processor.setMathMode(enabled);
        updateComparisonEngines();
        if (document.mathOcr != enabled) {
            document.mathOcr = enabled;
            if (!originalPath.isEmpty()) { externalDirty = true; refreshDirty(); }
        }
        languageEdit->setEnabled(!enabled);
        const bool tesseract = processor.engineId() == "tesseract";
        languageEdit->setReadOnly(!tesseract && processor.engineId() != "paddleocr-remote");
        languagePickerAction->setEnabled(tesseract);
        languagePickerButton->setEnabled(!enabled);
        recognizeAction->setText(enabled ? tr("Recognize math") : tr("Recognize"));
    });
    cancelButton = new QPushButton(tr("Cancel"), this);
    cancelButton->setObjectName(QStringLiteral("secondaryButton"));
    cancelButton->setEnabled(false);
    cancelRecognitionAction = toolbar->addWidget(cancelButton);
    cancelRecognitionAction->setObjectName("cancelRecognitionAction");
    cancelRecognitionAction->setVisible(false);
    connect(cancelButton, &QPushButton::clicked, &processor, &DocumentProcessor::cancel);
    cancelExportAction = toolbar->addAction(tr("Cancel export"));
    cancelExportAction->setObjectName("cancelExportAction");
    cancelExportAction->setVisible(false);
    connect(cancelExportAction, &QAction::triggered, this, [this] {
        if (exportBusy && exportCancel) {
            exportCancel->store(true);
            cancelExportAction->setEnabled(false);
            status->setText(tr("Cancelling export…"));
        }
    });
    addToolbarSeparator(toolbar);
    languagePickerButton = new QToolButton(this);
    languagePickerButton->setObjectName(QStringLiteral("languagePickerButton"));
    languagePickerButton->setPopupMode(QToolButton::InstantPopup);
    toolbar->addWidget(languagePickerButton);
    auto *languageMenu = new QMenu(languagePickerButton);
    languageMenu->setObjectName("ocrLanguageMenu");
    auto *languagePanel = new QWidget(languageMenu);
    auto *languageLayout = new QVBoxLayout(languagePanel);
    languageLayout->setContentsMargins(12, 10, 12, 10);
    languageLayout->addWidget(new QLabel(tr("OCR languages"), languagePanel));
    languageEdit = new QLineEdit(QStringLiteral("eng"), languagePanel);
    languageEdit->setToolTip(tr("Tesseract languages, for example eng+deu. The first language is primary."));
    languageEdit->setObjectName(QStringLiteral("languageField"));
    languageEdit->setMinimumWidth(260);
    languageLayout->addWidget(languageEdit);
    ocrLanguageStatus = new QLabel(tr("Check languages to recognize together. The first code is primary."), languagePanel);
    ocrLanguageStatus->setWordWrap(true);
    ocrLanguageStatus->setMaximumWidth(280);
    languageLayout->addWidget(ocrLanguageStatus);
    ocrLanguageList = new QListWidget(languagePanel);
    ocrLanguageList->setObjectName("availableOcrLanguages");
    ocrLanguageList->setMinimumHeight(180);
    languageLayout->addWidget(ocrLanguageList);
    auto *addLanguagePack = new QPushButton(tr("Add language…"), languagePanel);
    addLanguagePack->setObjectName("addToolbarOcrLanguage");
    languageLayout->addWidget(addLanguagePack);
    connect(addLanguagePack, &QPushButton::clicked, this, [this,languageMenu] {
        languageMenu->close();
        if (const auto codes = chooseAdditionalOcrLanguage(this,languageEdit->text())) {
            languageEdit->setText(*codes);
            refreshOcrLanguageList();
        }
    });
    auto *doneLanguages = new QPushButton(tr("Done"), languagePanel);
    languageLayout->addWidget(doneLanguages);
    auto *languageWidget = new QWidgetAction(languageMenu);
    languageWidget->setDefaultWidget(languagePanel);
    languageMenu->addAction(languageWidget);
    languagePickerButton->setMenu(languageMenu);
    connect(languageMenu, &QMenu::aboutToShow, this, [this, languageMenu, addLanguagePack] {
        addLanguagePack->setVisible(processor.engineId() == "tesseract");
        languageMenu->setProperty("previousLanguages", languageEdit->text());
        refreshOcrLanguageList();
    });
    connect(languageMenu, &QMenu::aboutToHide, this, [this, languageMenu] {
        const QString code = languageEdit->text().trimmed();
        languageEdit->setText(QRegularExpression("^[A-Za-z0-9_+/-]{1,64}$").match(code).hasMatch()
            ? code : languageMenu->property("previousLanguages").toString());
    });
    connect(doneLanguages, &QPushButton::clicked, languageMenu, &QMenu::close);
    connect(languageEdit, &QLineEdit::returnPressed, languageMenu, &QMenu::close);
    connect(ocrLanguageList, &QListWidget::itemChanged, this, [this](QListWidgetItem *item) {
        QStringList selected = languageEdit->text().split('+', Qt::SkipEmptyParts);
        const QString code = item->data(Qt::UserRole).toString();
        if (item->checkState() == Qt::Checked) { if (!selected.contains(code)) selected.append(code); }
        else selected.removeAll(code);
        languageEdit->setText(selected.join('+'));
    });
    const auto updateLanguageButton = [this] {
        const QString codes = languageEdit->text();
        languagePickerButton->setText(tr("Languages: %1").arg(codes));
        languagePickerButton->setToolTip(tr("OCR languages: %1. Select languages or enter a combination.").arg(codes));
        const QSignalBlocker blocker(ocrLanguageList);
        const auto selected = codes.split('+', Qt::SkipEmptyParts);
        for (int i = 0; i < ocrLanguageList->count(); ++i)
            ocrLanguageList->item(i)->setCheckState(selected.contains(ocrLanguageList->item(i)->data(Qt::UserRole).toString()) ? Qt::Checked : Qt::Unchecked);
    };
    connect(languageEdit, &QLineEdit::textChanged, this, updateLanguageButton);
    updateLanguageButton();
    fileMenu->addSeparator();
    auto *exportMenu = fileMenu->addMenu(tr("Export"));
    auto *scanMenu = exportMenu->addMenu(tr("Scans"));
    scanExportAction = scanMenu->menuAction();
    scanExportAction->setEnabled(false);
    scanMenu->addAction(tr("Current page as image…"), this, [this] {
        chooseExportPath(tr("Export scanned page"), tr("PNG image (*.png);;JPEG image (*.jpg *.jpeg)"), "png", ExportFormat::ScanImage);
    });
    scanMenu->addAction(tr("All pages as PDF…"), this, [this] {
        chooseExportPath(tr("Export scanned document"), tr("PDF files (*.pdf)"), "pdf", ExportFormat::ScanPdf);
    });
    auto *pdfMenu = exportMenu->addMenu(tr("PDF"));
    pdfAction = pdfMenu->addAction(tr("Export searchable PDF…"), this, [this] { exportPdf(false); });
    formattedPdfAction = pdfMenu->addAction(tr("Export formatted PDF…"), this, [this] { exportPdf(false, true); });
    formattedPdfAction->setObjectName(QStringLiteral("exportFormattedPdfAction"));
    formattedPdfAction->setToolTip(tr("Recreate corrected text and optionally typeset recognized formulas."));
    formattedPdfAction->setEnabled(false);
    pdfaAction = pdfMenu->addAction(tr("Export archival PDF/A…"), this, [this] { exportPdf(true); });
    convertDjVuAction = pdfMenu->addAction(tr("Convert DjVu to PDF…"), this, &MainWindow::convertDjVuToPdf);
    convertDjVuAction->setObjectName(QStringLiteral("convertDjVuAction"));
    convertDjVuAction->setEnabled(false);
    pdfAction->setEnabled(false);
    pdfaAction->setEnabled(false);

    auto *textMenu = exportMenu->addMenu(tr("Text and HTML"));
    textMenu->addAction(tr("Plain text…"), this, &MainWindow::exportText);
    textMenu->addAction(tr("Layout HTML…"), this, &MainWindow::exportHtml);
    textMenu->addAction(tr("Scientific book as HTML…"), this, &MainWindow::exportScientificBook);
    auto *latexExport=textMenu->addAction(tr("Document as LaTeX…"),this,&MainWindow::exportLatex);
    latexExport->setObjectName("exportLatexDocumentAction");
    auto *dataMenu = exportMenu->addMenu(tr("OCR data"));
    dataMenu->addAction(tr("Structured OCR as JSON…"), this, &MainWindow::exportJson);
    dataMenu->addAction(tr("Verified corrections…"), this, &MainWindow::exportVerifiedSamples);

    auto *splitter = new QSplitter(this);
    splitter->setObjectName(QStringLiteral("workspaceSplitter"));
    splitter->setHandleWidth(1);
    auto *navigationPane = new QWidget(splitter);
    navigationPane->setObjectName(QStringLiteral("navigationPane"));
    auto *navigationLayout = new QVBoxLayout(navigationPane);
    navigationLayout->setContentsMargins(14, 16, 14, 14);
    navigationLayout->setSpacing(10);
    auto *navigationSplitter = new QSplitter(Qt::Vertical, navigationPane);
    navigationSplitter->setObjectName(QStringLiteral("navigationSplitter"));
    navigationSplitter->setChildrenCollapsible(false);
    navigationSplitter->setHandleWidth(8);
    navigationLayout->addWidget(navigationSplitter, 1);
    auto *pagesPanel = new QWidget(navigationSplitter);
    auto *pagesLayout = new QVBoxLayout(pagesPanel);
    pagesLayout->setContentsMargins(0, 0, 0, 0);
    pagesLayout->setSpacing(10);
    auto *pagesHeading = new QLabel(tr("PAGES"), pagesPanel);
    pagesHeading->setObjectName(QStringLiteral("sectionHeading"));
    pagesLayout->addWidget(pagesHeading);
    pageList = new QListWidget(pagesPanel);
    pageList->setObjectName(QStringLiteral("pageMiniatures"));
    pageList->setViewMode(QListView::IconMode);
    pageList->setFlow(QListView::TopToBottom);
    pageList->setWrapping(false);
    pageList->setMovement(QListView::Static);
    pageList->setResizeMode(QListView::Adjust);
    pageList->setGridSize(QSize(170, 190));
    pageList->setIconSize(QSize(126, 156));
    pageList->setMinimumHeight(80);
    pageList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(pageList, &QListWidget::customContextMenuRequested, this, &MainWindow::showPageContextMenu);
    navigationPane->setMinimumWidth(195);
    navigationPane->setMaximumWidth(245);
    pagesLayout->addWidget(pageList, 1);
    auto *blocksPanel = new QWidget(navigationSplitter);
    auto *blocksLayout = new QVBoxLayout(blocksPanel);
    blocksLayout->setContentsMargins(0, 0, 0, 0);
    blocksLayout->setSpacing(10);
    auto *regionsHeading = new QLabel(tr("IMAGE REGIONS"), blocksPanel);
    regionsHeading->setObjectName(QStringLiteral("sectionHeading"));
    blocksLayout->addWidget(regionsHeading);
    auto *blockMiniatures = new BlockMiniatureList(blocksPanel);
    regionList = blockMiniatures;
    regionList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    regionList->setObjectName(QStringLiteral("imageRegions"));
    regionList->setToolTip(tr("Right-click a block miniature for recognition, language selection, or removal. "
        "Select a block and press Shift+F10 to open its menu from the keyboard."));
    regionList->setMinimumHeight(80);
    blocksLayout->addWidget(regionList, 1);
    splitBlocksButton = new QPushButton(tr("Smart split"), blocksPanel);
    splitBlocksButton->setIcon(QIcon(":/actions/smart-split.png"));
    splitBlocksButton->setIconSize(QSize(32, 20));
    splitBlocksButton->setObjectName("smartSplit");
    splitBlocksButton->setToolTip(tr("Detect text, math and mixed blocks using the OCR language."));
    splitBlocksButton->setEnabled(false);
    auto *splitRow = new QHBoxLayout;
    splitRow->addWidget(splitBlocksButton, 1);
    auto *splitOptions = new QToolButton(blocksPanel);
    splitOptions->setObjectName("smartSplitOptions");
    splitOptions->setToolTip(tr("More page-area detection options"));
    splitOptions->setArrowType(Qt::DownArrow);
    splitOptions->setPopupMode(QToolButton::InstantPopup);
    auto *splitMenu = new QMenu(splitOptions);
    splitMenu->addAction(aiPageAreasAction);
    splitMenu->addAction(aiSplitBlockAction);
    splitMenu->addAction(refineFormulaAction);
    splitOptions->setMenu(splitMenu);
    splitRow->addWidget(splitOptions);
    blocksLayout->addLayout(splitRow);
    connect(splitBlocksButton, &QPushButton::clicked, this, [this] {
        if (!maybeApplyEditor()) return;
        mathMode->setChecked(false);
        status->setText(tr("Detecting text and math blocks…"));
        processor.splitBlocks(imagePath, languageEdit->text(), currentPage);
    });
    deleteRegionButton = new QPushButton(tr("Delete region"), blocksPanel);
    deleteRegionButton->setIcon(removeBlockIcon());
    deleteRegionButton->setObjectName(QStringLiteral("deleteRegionButton"));
    deleteRegionButton->setEnabled(false);
    blocksLayout->addWidget(deleteRegionButton);
    navigationSplitter->setStretchFactor(0, 1);
    navigationSplitter->setStretchFactor(1, 2);
    navigationSplitter->setSizes({280, 400});
    navigationSplitter->handle(1)->setCursor(Qt::SplitVCursor);
    navigationSplitter->handle(1)->setToolTip(tr("Drag to resize page and block thumbnails"));
    pageList->setToolTip(tr("Select a page miniature to preview it."));
    view = new PageView(splitter);
    view->setObjectName(QStringLiteral("pagePreview"));
    scene = new QGraphicsScene(this);
    view->setScene(scene);
    view->setAcceptDrops(true);
    view->setToolTip(tr("Drag to draw a region. Shift-click regions to toggle selection. Click empty space to clear it. Middle-drag to pan."));
    view->setRenderHint(QPainter::Antialiasing);
    view->setBackgroundBrush(QColor(QStringLiteral("#e9eef5")));
    auto *imageTools = view->imageTools();
    imageTools->addSeparator();
    rotateLeftAction = imageTools->addAction(QIcon(":/actions/rotate-left.png"), tr("Rotate image left"));
    rotateLeftAction->setObjectName("rotateImageLeftAction");
    rotateRightAction = imageTools->addAction(QIcon(":/actions/rotate-right.png"), tr("Rotate image right"));
    rotateRightAction->setObjectName("rotateImageRightAction");
    const auto rotateImage = [this](int turns) {
        if (processor.isBusy() || exportBusy || projectLoading || imagePath.isEmpty() || !maybeApplyEditor()) return;
        auto settings = displayedPreprocessing;
        settings.quarterTurns = (settings.quarterTurns+turns+4)%4;
        applyPagePreprocessing(settings, true);
    };
    connect(rotateLeftAction, &QAction::triggered, this, [rotateImage] { rotateImage(-1); });
    connect(rotateRightAction, &QAction::triggered, this, [rotateImage] { rotateImage(1); });
    correctSkewAction = imageTools->addAction(QIcon(":/actions/correct-skew.png"), tr("Correct skew"));
    correctSkewAction->setObjectName("correctSkewAction");
    correctSkewAction->setToolTip(tr("Detect and straighten tilted text lines on the current page. This can be undone."));
    connect(correctSkewAction, &QAction::triggered, this, &MainWindow::correctCurrentPageSkew);
    undoSkewAction = imageTools->addAction(QIcon::fromTheme(QStringLiteral("edit-undo"),
        style()->standardIcon(QStyle::SP_ArrowBack)), tr("Undo skew correction"));
    undoSkewAction->setObjectName("undoSkewCorrectionAction");
    undoSkewAction->setToolTip(tr("Undo skew correction and restore the previous image, text, and blocks."));
    connect(undoSkewAction, &QAction::triggered, this, [this] {
        if (processor.isBusy() || exportBusy || projectLoading || imagePath.isEmpty() || !maybeApplyEditor()) return;
        if (canUndoSkewCorrection()) undoStack.undo();
    });
    imageSplitAction = imageTools->addAction(QIcon(":/actions/smart-split.png"), tr("Smart split"));
    imageSplitAction->setObjectName("imageSmartSplitAction");
    connect(imageSplitAction, &QAction::triggered, splitBlocksButton, &QPushButton::click);
    if (auto *button = qobject_cast<QToolButton *>(imageTools->widgetForAction(imageSplitAction))) {
        button->setObjectName("imageSmartSplitButton");
        auto *menu = new QMenu(button);
        menu->addAction(aiPageAreasAction);
        menu->addAction(aiSplitBlockAction);
        menu->addAction(refineFormulaAction);
        button->setMenu(menu);
        button->setPopupMode(QToolButton::MenuButtonPopup);
    }
    imageClearBlocksAction = imageTools->addAction(QIcon(":/actions/clear-blocks.png"), tr("Clear all blocks"));
    imageClearBlocksAction->setObjectName("imageClearBlocksAction");
    connect(imageClearBlocksAction, &QAction::triggered, this, [this] { clearPageBlocks(currentPage); });
    for (auto *action : {rotateLeftAction, rotateRightAction, correctSkewAction, imageSplitAction, imageClearBlocksAction})
        qobject_cast<QToolButton *>(imageTools->widgetForAction(action))->setIconSize(QSize(32, 24));
    imageRecognizeBlockAction = imageTools->addAction(recognizeActionIcon(), tr("Recognize selected blocks"));
    imageRecognizeBlockAction->setObjectName("imageRecognizeBlockAction");
    connect(imageRecognizeBlockAction, &QAction::triggered, this, &MainWindow::runOcr);
    imageBlockOptionsAction = imageTools->addAction(style()->standardIcon(QStyle::SP_FileDialogContentsView), tr("Block options"));
    imageBlockOptionsAction->setObjectName("imageBlockOptionsAction");
    connect(imageBlockOptionsAction, &QAction::triggered, this, [this] {
        auto *button = view->imageTools()->widgetForAction(imageBlockOptionsAction);
        showBlockContextMenu(selectedImageRegion, button->mapToGlobal(button->rect().bottomLeft()));
    });
    imageRemoveBlockAction = imageTools->addAction(removeBlockIcon(), tr("Remove selected block"));
    imageRemoveBlockAction->setObjectName("imageRemoveBlockAction");
    connect(imageRemoveBlockAction, &QAction::triggered, this, [this] { deleteImageRegion(selectedImageRegion); });
    refreshImageToolbar();
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
    editFormulaAction = formatToolbar->addAction(tr("Edit formula…"), this, &MainWindow::editSelectedFormula);
    editFormulaAction->setObjectName("editFormulaAction");
    editFormulaAction->setToolTip(tr("Edit the selected equation as LaTeX with a live preview."));
    editFormulaAction->setEnabled(false);
    auto *editorSplitter = new QSplitter(Qt::Vertical, textPanel);
    editorSplitter->setObjectName(QStringLiteral("textEditorSplitter"));
    editorSplitter->setChildrenCollapsible(false);
    editorSplitter->setHandleWidth(8);
    textLayout->addWidget(editorSplitter, 1);
    selectionEditor = new QTextEdit(editorSplitter);
    selectionEditor->setObjectName(QStringLiteral("recognizedTextEditor"));
    selectionEditor->setPlaceholderText(tr("Select words on the page or drag a region, then edit here."));
    selectionEditor->setAcceptRichText(true);
    selectionEditor->setMinimumHeight(120);
    spellHighlighter = new SpellHighlighter(selectionEditor);
    selectionEditor->addAction(teachSymbolAction);
    selectionEditor->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(selectionEditor, &QTextEdit::selectionChanged, this, [this] {
        teachSymbolAction->setEnabled(!processor.isBusy() && !exportBusy && !projectLoading &&
            !imagePath.isEmpty() && !editorPending && !selectedWords.isEmpty());
    });
    connect(selectionEditor, &QTextEdit::customContextMenuRequested, this, [this](const QPoint &position) {
        std::unique_ptr<QMenu> menu(selectionEditor->createStandardContextMenu());
        menu->addSeparator(); menu->addAction(teachSymbolAction);
        menu->exec(selectionEditor->viewport()->mapToGlobal(position));
    });
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
    editorSplitter->handle(1)->setCursor(Qt::SplitVCursor);
    editorSplitter->handle(1)->setToolTip(tr("Drag to resize recognized text and the word list"));
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
    addToolbarSeparator(formatToolbar);
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
    connect(view, &PageView::pageContextMenuRequested, this, [this](const QPoint &position) { showPageActions(currentPage, position); });
    connect(blockMiniatures, &BlockMiniatureList::blockContextMenuRequested, this, &MainWindow::showBlockContextMenu);
    connect(regionList, &QListWidget::itemSelectionChanged, this, [this] {
        QSet<QString> ids;
        for (const auto *item : regionList->selectedItems())
            ids.insert(item->data(Qt::UserRole).toString());
        if (ids == selectedRegionIds) return;
        const int row = regionList->currentRow();
        if (!maybeApplyEditor()) { refreshImageRegionSelection(false); return; }
        selectedRegionIds = std::move(ids);
        selectedImageRegion = row;
        refreshImageRegionSelection(true);
    });
    connect(deleteRegionButton, &QPushButton::clicked, this, [this] {
        deleteImageRegion(selectedImageRegion);
    });
    connect(applyButton, &QPushButton::clicked, this, &MainWindow::applyCorrection);
    connect(regionOcrButton, &QPushButton::clicked, this, &MainWindow::runOcr);
    connect(reviewWordButton, &QPushButton::clicked, this, &MainWindow::reviewSelectedWord);
    connect(selectionEditor, &QTextEdit::textChanged, this, [this] { editorPending = true; teachSymbolAction->setEnabled(false); });
    connect(view, &PageView::wordClicked, this, &MainWindow::selectWord);
    connect(wordList, &QListWidget::itemSelectionChanged, this, [this] {
        QStringList ids;
        for (QListWidgetItem *item : wordList->selectedItems()) {
            const int row = wordList->row(item);
            if (row >= 0 && row < displayedWords.size()) ids.append(displayedWords.at(row)->id);
        }
        if (!maybeApplyEditor()) {
            const QSignalBlocker blocker(wordList);
            wordList->clearSelection();
            for (int index : selectedWords) wordList->item(index)->setSelected(true);
            return;
        }
        updateSelection(indicesForIds(ids));
    });
    status = new QLabel(tr("Open an image, PDF or DjVu to begin."), this);
    statusBar()->addPermanentWidget(status);

    connect(&processor, &DocumentProcessor::busyChanged, this, &MainWindow::setBusy);
    connect(&processor, &DocumentProcessor::recognitionStage, this,
        [this](int page,const QString &stage,int complete,int total) {
            if (!processor.isRecognizing()) return;
            const QString detail=total>0 ? tr("%1 · %2 / %3").arg(stage).arg(complete).arg(total) : stage;
            status->setText(tr("Page %1: %2").arg(page).arg(detail));
            if (recognitionProgress && recognitionProgress->isVisible()) recognitionProgress->setLabelText(tr("Page %1: %2").arg(page).arg(detail));
        });
    connect(&processor, &DocumentProcessor::failed, this, [this](const QString &message) {
        queuedImageRegions.clear();
        verificationTarget.reset();
        refreshRenderedPageIfNeeded();
        showError(message);
    });
    connect(&processor, &DocumentProcessor::cancelled, this, [this] {
        queuedImageRegions.clear();
        verificationTarget.reset();
        status->setText(tr("Cancelled"));
        refreshRenderedPageIfNeeded();
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
    connect(&processor, &DocumentProcessor::blocksSplit, this, [this](OcrPage split) {
        const auto *old = document.page(currentPage);
        const QString language = split.language;
        auto blocks = std::move(split.imageRegions);
        OcrPage page = old ? *old : std::move(split);
        if (!old) {
            page.engineId.clear();
            page.sourceWidth = rawImage.width();
            page.sourceHeight = rawImage.height();
            page.preprocessing = displayedPreprocessing;
        }
        page.imageRegions = std::move(blocks);
        page.language = language;
        selectedRegionIds.clear();
        selectedImageRegion = -1;
        const auto count = page.imageRegions.size();
        undoStack.push(new ReplacePageCommand(document, currentPage, std::move(page), tr("Smart split")));
        status->setText(tr("%1 blocks detected. Review their types, then click Recognize.").arg(count));
    });
    connect(&processor,&DocumentProcessor::recognitionHistoryReady,this,[this](const OcrPage &evidence) {
        const auto *previous=document.page(evidence.number);
        auto page=previous?*previous:evidence;
        core::mergeRecognitionHistory(page.recognitionHistory,evidence.recognitionHistory);
        document.sourcePath=originalPath;document.totalPages=pageCount;
        undoStack.push(new ReplacePageCommand(document,evidence.number,std::move(page),tr("Save recognition history")));
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
        if(oldPage && oldPage->coordinates.isDefined())newPage.coordinates=oldPage->coordinates;
        else {
            auto coordinates=ImageProcessor::coordinatePipeline(rawImage.size(),displayedPreprocessing,
                processing::pageSourceKind(originalPath)==processing::PageSourceKind::Pdf?processor.renderedDpi(currentPage):0);
            if(!coordinates){showError(coordinates.error());return;}newPage.coordinates=*coordinates;
        }
        for (OcrWord &word : newPage.words) {
            word.bounds.translate(offset);
            for (QRect &fragment : word.sourceBoxes) fragment.translate(offset);
            if (!region.isEmpty()) { word.id.clear(); word.sourceWordIds.clear(); }
        }
        core::translateOcrReadings(newPage.ocrReadings,offset);
        for (auto &result : newPage.recognitionEvidence) {
            auto &area = result.provenance.inputArea;
            if (area.isEmpty()) area = QRect(QPoint(), result.provenance.imageSize);
            area.translate(offset);
        }
        core::attachRecognitionCoordinates(newPage);
        if (region.isEmpty()) {
            if (oldPage) newPage.words.removeIf([&](const OcrWord &word) {
                return std::ranges::any_of(oldPage->imageRegions, [&](const ImageRegion &block) {
                    return block.notText && overlapsWord(word, block.bounds);
                });
            });
            carryCorrections(newPage, oldPage, document.preserveFormatting,
                !document.useRawOcrOutput || newPage.engineId != "tesseract");
        } else if (oldPage) {
            auto previous = *oldPage;
            previous.words.removeIf([&](const auto &word) { return !overlapsWord(word, region); });
            previous.pendingCorrections.removeIf([&](const auto &word) { return !overlapsWord(word, region); });
            previous.deletedWords.removeIf([&](const auto &word) { return !overlapsWord(word, region); });
            newPage.width = size.width(); newPage.height = size.height();
            core::retainCorrections(newPage, previous, document.preserveFormatting);
            core::normalizeBlockFontSizes(newPage, {}, true);
        }
        document.sourcePath = originalPath;
        document.engine = newPage.engineId;
        document.language = newPage.language;
        document.totalPages = pageCount;
        if (region.isEmpty()) {
            if (!document.useRawOcrOutput || newPage.engineId!="tesseract") core::setFormulaOutput(newPage,document.unicodeFormulas);
            core::normalizeBlockFontSizes(newPage);
            undoStack.push(new ReplacePageCommand(document, currentPage, std::move(newPage), tr("Recognize page")));
        } else {
            OcrPage merged;
            if (oldPage) merged = *oldPage;
            for (const auto &detected : newPage.imageRegions) {
                auto target = std::ranges::find(merged.imageRegions, detected.id, &ImageRegion::id);
                if (target != merged.imageRegions.end() && target->content.isEmpty())
                    target->content = detected.content;
            }
            merged.id = QStringLiteral("page-%1").arg(currentPage);
            merged.number = currentPage;
            merged.width = size.width();
            merged.height = size.height();
            if (!oldPage) {
                merged.sourceWidth = rawImage.width();
                merged.sourceHeight = rawImage.height();
            }
            merged.renderDpi = newPage.renderDpi;
            merged.pendingCorrections.removeIf([&](const auto &word) { return overlapsWord(word, region); });
            merged.pendingCorrections += newPage.pendingCorrections;
            merged.engineId = newPage.engineId;
            merged.language = newPage.language;
            merged.coordinates=newPage.coordinates;
            processing::appendRecognitionEvidence(merged, newPage);
            core::mergeOcrReadings(merged.ocrReadings,std::move(newPage.ocrReadings),region);
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
            if (!document.useRawOcrOutput || merged.engineId!="tesseract") core::setFormulaOutput(merged,document.unicodeFormulas);
            undoStack.push(new ReplacePageCommand(document, currentPage, std::move(merged), tr("Recognize region")));
        }
        showDocument();
        if (!queuedImageRegions.isEmpty()) {
            ++nextQueuedImageRegion;
            status->setText(tr("Recognized region %1 of %2")
                            .arg(nextQueuedImageRegion).arg(queuedImageRegions.size()));
            runNextImageRegion();
        } else if (const auto *recognized = document.page(currentPage); recognized->engineId == "local-mixed") {
            const auto math = std::ranges::count_if(recognized->words, [](const auto &word) { return word.layoutBlock; });
            status->setText(tr("Page %1 · %2 blocks · %3 text words · %4 formulas")
                .arg(currentPage).arg(recognized->imageRegions.size()).arg(recognized->words.size() - math).arg(math));
        } else status->setText((mathMode->isChecked() ? tr("Page %1: %2 formulas recognized")
                                                   : tr("Page %1: %2 words recognized"))
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
        carryCorrections(page, oldPage, document.preserveFormatting,
            !document.useRawOcrOutput || page.engineId != "tesseract");
        if (!document.useRawOcrOutput || page.engineId!="tesseract") core::setFormulaOutput(page,document.unicodeFormulas);
        const QString language = page.language;
        const QString engineId = page.engineId;
        core::normalizeBlockFontSizes(page);
        undoStack.push(new ReplacePageCommand(document, number, std::move(page), tr("Recognize page")));
        document.engine = engineId;
        document.language = language;
        document.totalPages = pageCount;
        if (number == currentPage) showDocument();
    });
    connect(&processor, &DocumentProcessor::batchProgress, this, [this](qsizetype completed, qsizetype total) {
        status->setText(tr("Batch OCR: %1 of %2 pages complete").arg(completed).arg(total));
        updateRecognitionProgress(completed, total);
    });
    connect(&processor, &DocumentProcessor::batchFinished, this, [this] {
        status->setText(tr("Batch OCR complete"));
        refreshRenderedPageIfNeeded();
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
        syncNativeOcrModel();
        { const QSignalBlocker blocker(unicodeFormulasAction); unicodeFormulasAction->setChecked(document.unicodeFormulas); }
        correctionMemory.reset();
        const OcrPage *page = document.page(currentPage);
        if (page && processing::isPagedSource(originalPath) && !rawImagePath.isEmpty() && page->renderDpi != processor.renderedDpi(currentPage))
            showPage(currentPage);
        else if (!rawImagePath.isEmpty() && (page ? page->preprocessing != displayedPreprocessing
                                                      : !displayedPreprocessing.isIdentity()))
            loadImage(rawImagePath);
        else showDocument();
        refreshDirty();
        undoAction->setEnabled(!processor.isBusy() && !exportBusy && undoStack.canUndo());
        redoAction->setEnabled(!processor.isBusy() && !exportBusy && undoStack.canRedo());
    });
    ocrServices=processing::readOcrServices(QJsonDocument::fromJson(QSettings("YAGF","YAGF Revival").value("ocr/serviceLibrary").toByteArray()).array());
    const auto savedPrimary=QJsonDocument::fromJson(QSettings("YAGF","YAGF Revival").value("ocr/primaryService").toByteArray()).object();
    const auto primaries=processing::readOcrServices(QJsonArray{savedPrimary});
    if (!primaries.isEmpty() && primaries.first().supportsText()) {
        const auto &service=primaries.first();
        auto restored=processing::makeOcrServiceEngine(service);
        if (restored) {
            processor.setEngine(std::move(*restored));
            const bool local=service.provider=="tesseract";
            languageEdit->setText(service.language.isEmpty() ? (local ? QStringLiteral("eng") : QStringLiteral("auto")) : service.language);
            languageEdit->setReadOnly(!local && service.provider!="paddleocr");
            languagePickerAction->setEnabled(local);
        }
    }
    const auto savedFormula=QJsonDocument::fromJson(QSettings("YAGF","YAGF Revival").value("ocr/formulaService").toByteArray()).object();
    const auto formulas=processing::readOcrServices(QJsonArray{savedFormula});
    if (!formulas.isEmpty()) {
        const auto configured=processor.setFormulaOcrService(formulas.first());
        if (!configured) qWarning("Could not restore formula OCR engine: %s",qPrintable(configured.error()));
    }
    updatePageLayoutService();
    updateComparisonEngines();
    // Editors consume PageUp/PageDown as scrolling keys before ordinary shortcuts.
    qApp->installEventFilter(this);
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::KeyPress) {
        const auto *widget = qobject_cast<QWidget *>(watched);
        const auto *key = static_cast<QKeyEvent *>(event);
        if (widget && widget->window() == this && key->modifiers() == Qt::NoModifier &&
            (key->key() == Qt::Key_PageUp || key->key() == Qt::Key_PageDown)) {
            // Reuse navigation's busy/boundary checks and pending-edit handling.
            (key->key() == Qt::Key_PageUp ? previousButton : nextButton)->click();
            event->accept();
            return true;
        }
    }
    return QMainWindow::eventFilter(watched, event);
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
    if (!busy && recognitionProgress) {
        recognitionProgress->hide();
        recognitionProgress->reset();
    }
    const bool blocked = busy || exportBusy || projectLoading;
    verification->setBusy(blocked);
    symbolRulesAction->setEnabled(!blocked && !originalPath.isEmpty());
    teachSymbolAction->setEnabled(!blocked && !imagePath.isEmpty() && !editorPending && !selectedWords.isEmpty());
    scanAction->setEnabled(!blocked);
    mathMode->setEnabled(!blocked);
    unicodeFormulasAction->setEnabled(!blocked);
    splitBlocksButton->setEnabled(!blocked && !imagePath.isEmpty());
    renderingAction->setEnabled(!blocked && processing::isPagedSource(originalPath));
    undoAction->setEnabled(!blocked && undoStack.canUndo());
    redoAction->setEnabled(!blocked && undoStack.canRedo());
    recognizeAction->setEnabled(!blocked && !imagePath.isEmpty());
    languagePickerButton->setEnabled(!blocked && !mathMode->isChecked());
    languageEdit->setEnabled(!blocked && !mathMode->isChecked());
    languagePickerAction->setEnabled(!blocked && processor.engineId() == "tesseract");
    view->setEnabled(!blocked);
    cancelRecognitionAction->setVisible(processor.isRecognizing());
    cancelButton->setEnabled(processor.isRecognizing());
    cancelExportAction->setVisible(exportBusy);
    cancelExportAction->setEnabled(exportBusy);
    selectionEditor->setEnabled(!blocked);
    const auto *formulaPage = document.page(currentPage);
    editFormulaAction->setEnabled(!blocked && !imagePath.isEmpty() && formulaPage &&
        !processing::detail::editableFormulaSpans(*formulaPage, selectedWords).isEmpty());
    applyButton->setEnabled(!blocked && !selectedWords.isEmpty() && !selectionEditor->isReadOnly());
    const auto *activePage = document.page(currentPage);
    const bool textRegion = !activePage || selectedImageRegion < 0 || selectedImageRegion >= activePage->imageRegions.size() ||
                            !activePage->imageRegions[selectedImageRegion].notText;
    regionOcrButton->setEnabled(!blocked && textRegion && !imagePath.isEmpty() && selectedRegionIds.size() == 1);
    reviewWordButton->setEnabled(!blocked && !imagePath.isEmpty() && canReviewSelection());
    regionList->setEnabled(!blocked);
    deleteRegionButton->setEnabled(!blocked && selectedImageRegion >= 0);
    improveAction->setEnabled(!blocked && !rawImage.isNull());
    restoreAction->setEnabled(!blocked && !rawImage.isNull());
    layoutAction->setEnabled(!blocked && document.page(currentPage) && !document.page(currentPage)->regions.isEmpty());
    createRegionAction->setEnabled(!blocked && !selectedWords.isEmpty() &&
        document.page(currentPage) && selectedWords.size() < document.page(currentPage)->words.size());
    pdfAction->setEnabled(!blocked && document.wordCount() > 0);
    pdfaAction->setEnabled(!blocked && document.wordCount() > 0);
    refreshImageToolbar();
    updatePageControls();
}

void MainWindow::updatePageControls()
{
    const QSignalBlocker blocker(pageSpin);
    pageSpin->setMaximum(pageCount);
    pageSpin->setValue(currentPage);
    const bool available = !processor.isBusy() && !exportBusy && !projectLoading;
    if (nativeTrainingAction) nativeTrainingAction->setEnabled(available && !originalPath.isEmpty());
    if (ocrServicesAction) ocrServicesAction->setEnabled(available);
    if (ocrReadingsAction) ocrReadingsAction->setEnabled(available && !imagePath.isEmpty() && document.page(currentPage) && (!document.page(currentPage)->ocrReadings.isEmpty() || !document.page(currentPage)->recognitionHistory.runs.isEmpty()));
    if (renderingAction) renderingAction->setEnabled(available && processing::isPagedSource(originalPath));
    if (preserveFormattingAction) preserveFormattingAction->setEnabled(available);
    if (rawOcrOutputAction) rawOcrOutputAction->setEnabled(available);
    if (scanExportAction) scanExportAction->setEnabled(available && !imagePath.isEmpty());
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
    if (batchAction) batchAction->setEnabled(available && !imagePath.isEmpty());
    if (remainingAction) remainingAction->setEnabled(available && !imagePath.isEmpty() &&
        processing::isPagedSource(originalPath) && pageCount > 1);
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
    finishProjectLoading();
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
    if (processor.isBusy() || exportBusy || projectLoading) return;
    const QFileInfo source(path);
    if (!source.isFile() || (!processing::isPagedSource(path) &&
                              !QImageReader(path).canRead())) {
        showError(tr("This file is not a readable image, PDF or DjVu: %1").arg(path));
        return;
    }
    if (!maybeSave()) return;
    processor.resetSource();
    verificationTarget.reset();
    originalPath = QFileInfo(path).absoluteFilePath();
    projectPath.clear();
    document = {};
    syncNativeOcrModel();
    document.preserveFormatting = preserveFormattingAction->isChecked();
    document.useRawOcrOutput = rawOcrOutputAction->isChecked();
    document.mathOcr = mathMode->isChecked();
    document.unicodeFormulas=unicodeFormulasAction->isChecked();
    correctionMemory.reset();
    document.sourcePath = originalPath;
    undoStack.clear();
    externalDirty = false;
    dirty = false;
    updateTitle();
    wordList->clear();
    displayedWords.clear();
    selectedWords.clear();
    selectedRegionIds.clear();
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
    recognizeAction->setEnabled(false);
    improveAction->setEnabled(false);
    restoreAction->setEnabled(false);
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
    if (processor.isBusy() || exportBusy || projectLoading || path.isEmpty()) return;
    if (!maybeSave()) return;
    projectLoading = true;
    if (!projectLoadingProgress) {
        projectLoadingProgress = new ProjectLoadingDialog(this);
        projectLoadingProgress->setObjectName("projectLoadingProgress");
        projectLoadingProgress->setWindowTitle(tr("Opening project"));
        projectLoadingProgress->setCancelButton(nullptr);
        projectLoadingProgress->setWindowFlag(Qt::WindowCloseButtonHint, false);
        projectLoadingProgress->setWindowModality(Qt::WindowModal);
        projectLoadingProgress->setMinimumDuration(0);
        projectLoadingProgress->setRange(0, 0);
    }
    projectLoadingProgress->setLabelText(tr("Reading project and restoring pages…"));
    projectLoadingProgress->show();
    setBusy(false);
    const bool portable = QFileInfo(path).isDir() || path.endsWith(QStringLiteral(".yagfproj"), Qt::CaseInsensitive);
    struct LoadedProject { OcrDocument document; QString error; bool opened; };
    QFutureWatcher<LoadedProject> loading;
    QEventLoop loadingEvents;
    connect(&loading, &QFutureWatcher<LoadedProject>::finished, &loadingEvents, &QEventLoop::quit);
    loading.setFuture(QtConcurrent::run([path, portable] {
        LoadedProject result;
        result.opened = portable ? ProjectFile::loadPortable(path, &result.document, &result.error)
                                 : ProjectFile::load(path, &result.document, &result.error);
        return result;
    }));
    loadingEvents.exec();
    auto result = loading.result();
    if (!result.opened) { showError(result.error); return; }
    OcrDocument loaded = std::move(result.document);
    if (!QFileInfo::exists(loaded.sourcePath)) {
        showError(tr("Source document is missing: %1").arg(loaded.sourcePath));
        return;
    }
    processor.resetSource();
    undoStack.clear();
    originalPath = loaded.sourcePath;
    projectPath = path;
    rememberProject();
    verificationTarget.reset();
    document = std::move(loaded);
    syncNativeOcrModel();
    {
        const QSignalBlocker blocker(preserveFormattingAction);
        preserveFormattingAction->setChecked(document.preserveFormatting);
    }
    {
        const QSignalBlocker blocker(rawOcrOutputAction);
        rawOcrOutputAction->setChecked(document.useRawOcrOutput);
    }
    correctionMemory.reset();
    externalDirty = false;
    mathMode->setChecked(document.mathOcr);
    { const QSignalBlocker blocker(unicodeFormulasAction); unicodeFormulasAction->setChecked(document.unicodeFormulas); }
    if (const auto *first = document.page(1); first && !mathMode->isChecked() &&
        std::ranges::any_of(first->imageRegions, [](const auto &r) { return !r.content.isEmpty(); }) &&
        !first->language.isEmpty())
        languageEdit->setText(first->language);
    else if (!mathMode->isChecked() && (document.engine == processor.engineId() || document.engine == "local-mixed") && !document.language.isEmpty())
        languageEdit->setText(document.engine == "tesseract"
            ? processing::TesseractEngine::modelLanguage(document.language) : document.language);
    dirty = false;
    updateTitle();
    wordList->clear();
    displayedWords.clear();
    selectedWords.clear();
    selectedRegionIds.clear();
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
    restoreAction->setEnabled(false);
    layoutAction->setEnabled(false);
    createRegionAction->setEnabled(false);
    batchAction->setEnabled(false);
    pdfAction->setEnabled(false);
    pdfaAction->setEnabled(false);
    currentPage = 1;
    pageCount = 1;
    updatePageControls();
    if (processing::isPagedSource(originalPath)) {
        projectLoadingProgress->setLabelText(tr("Rendering the first page…"));
        status->setText(tr("Reading project source document…"));
        processor.openDocument(originalPath);
    } else {
        loadImage(originalPath);
    }
}

void MainWindow::finishProjectLoading()
{
    if (!projectLoading) return;
    projectLoading = false;
    projectLoadingProgress->hide();
    setBusy(processor.isBusy());
}

void MainWindow::rememberProject()
{
    QSettings settings(QStringLiteral("YAGF"), QStringLiteral("YAGF Revival"));
    const QString path = QDir::cleanPath(QFileInfo(projectPath).absoluteFilePath());
    QStringList recent = settings.value("projects/recent").toStringList();
    recent.removeAll(path);
    recent.prepend(path);
    settings.setValue("projects/recent", recent.mid(0, 10));
}

void MainWindow::refreshRecentProjects()
{
    recentProjectsMenu->clear();
    QSettings settings(QStringLiteral("YAGF"), QStringLiteral("YAGF Revival"));
    const QStringList paths = settings.value("projects/recent").toStringList();
    if (paths.isEmpty()) {
        recentProjectsMenu->addAction(tr("No recent projects"))->setEnabled(false);
        return;
    }
    for (const QString &path : paths) {
        const bool exists = QFileInfo::exists(path);
        QString label = QDir::toNativeSeparators(path);
        if (!exists) label = tr("%1 (missing)").arg(label);
        auto *action = recentProjectsMenu->addAction(label.replace('&', "&&"), this, [this, path] { loadProject(path); });
        action->setEnabled(exists && !processor.isBusy() && !exportBusy);
        action->setData(path);
    }
    recentProjectsMenu->addSeparator();
    recentProjectsMenu->addAction(tr("Clear recent projects"), this, [] {
        QSettings(QStringLiteral("YAGF"), QStringLiteral("YAGF Revival")).remove("projects/recent");
    });
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
    rememberProject();
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
    rememberProject();
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
    rememberProject();
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
    const int dpi = processing::isPagedSource(originalPath) ? processor.renderedDpi(currentPage) : 150;
    if (page && page->renderDpi != dpi && processing::isPagedSource(originalPath)) {
        const double ratio = double(dpi) / page->renderDpi;
        if (std::abs(source.width() - page->sourceWidth * ratio) > 2 ||
            std::abs(source.height() - page->sourceHeight * ratio) > 2) {
            showError(tr("The source dimensions changed independently of the rendering resolution.")); return;
        }
        const auto scaledImage = ImageProcessor::apply(source, page->preprocessing);
        if (!scaledImage) { showError(scaledImage.error()); return; }
        auto prior=*page;
        if(!prior.coordinates.isDefined()) {
            auto old=ImageProcessor::coordinatePipeline({page->sourceWidth,page->sourceHeight},page->preprocessing,
                processing::pageSourceKind(originalPath)==processing::PageSourceKind::Pdf?page->renderDpi:0);
            if(!old){showError(old.error());return;}prior.coordinates=*old;
        }
        auto destination=ImageProcessor::coordinatePipeline(source.size(),page->preprocessing,
            processing::pageSourceKind(originalPath)==processing::PageSourceKind::Pdf?dpi:0);
        if(!destination){showError(destination.error());return;}
        auto scaled=core::remapPageGeometry(prior,*destination);
        if(!scaled){showError(scaled.error());return;}
        scaled->sourceWidth=source.width();scaled->sourceHeight=source.height();scaled->renderDpi=dpi;
        document.replacePage(std::move(*scaled));
        page = document.page(currentPage);
        externalDirty = true; refreshDirty();
    }
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
    if(page && !page->coordinates.isDefined()) {
        auto mapping=ImageProcessor::coordinatePipeline(source.size(),settings,
            processing::pageSourceKind(originalPath)==processing::PageSourceKind::Pdf?dpi:0);
        if(!mapping){showError(mapping.error());return;}
        auto migrated=*page;migrated.coordinates=*mapping;core::attachRecognitionCoordinates(migrated);
        document.replacePage(std::move(migrated));page=document.page(currentPage);
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
    status->setText(tr("Page ready: %1 × %2 · %3 DPI%4").arg(image.width()).arg(image.height()).arg(dpi)
                    .arg(settings.isIdentity() ? QString() : tr(" · improved")));
    finishVerification();
    finishProjectLoading();
}

void MainWindow::showPage(int number)
{
    if (number < 1 || number > pageCount || processor.isBusy() || exportBusy) return;
    if (!maybeApplyEditor()) return;
    currentPage = number;
    updatePageControls();
    if (const OcrPage *page = document.page(number);
        page && !mathMode->isChecked() && (page->engineId == processor.engineId() || page->engineId == "local-mixed") && !page->language.isEmpty())
        languageEdit->setText(page->engineId == "tesseract"
            ? processing::TesseractEngine::modelLanguage(page->language) : page->language);
    wordList->clear();
    displayedWords.clear();
    selectedWords.clear();
    selectedRegionIds.clear();
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
    recognizeAction->setEnabled(false);
    improveAction->setEnabled(false);
    restoreAction->setEnabled(false);
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
    const auto base = ImageProcessor::apply(rawImage, {.restoredPng = current.restoredPng});
    if (!base) { showError(base.error()); return; }
    PreprocessDialog dialog(*base, current, this, processor.engineId() == QStringLiteral("tesseract")
        ? languageEdit->text().trimmed() : QStringLiteral("eng"));
    if (dialog.exec() != QDialog::Accepted) return;
    auto proposed = dialog.settings();
    proposed.restoredPng = current.restoredPng;
    proposed.restorationGeometry=current.restorationGeometry;
    proposed.perspective=current.perspective;
    if (proposed == current) return;
    const bool geometryChanged = std::abs(proposed.deskewDegrees - current.deskewDegrees) >= 0.01 ||
                                 proposed.quarterTurns != current.quarterTurns || proposed.bendPercent != current.bendPercent;
    applyPagePreprocessing(proposed, geometryChanged);
}

void MainWindow::correctCurrentPageSkew()
{
    if (imagePath.isEmpty() || processor.isBusy() || exportBusy || projectLoading || !maybeApplyEditor()) return;
    const auto assessment = ImageProcessor::assess(QImage(imagePath));
    if (!assessment) { showError(assessment.error()); return; }
    if (std::abs(assessment->skewDegrees) < .1) {
        statusBar()->showMessage(tr("No additional skew detected on this page."), 5000);
        return;
    }
    // Assess the displayed, already preprocessed image. Add its remaining tilt
    // to the saved angle, preserving rotation, restoration and other filters.
    auto settings = displayedPreprocessing;
    settings.deskewDegrees = std::round((settings.deskewDegrees+assessment->skewDegrees)*10)/10;
    if (std::abs(settings.deskewDegrees) > 15) {
        showError(tr("The detected correction exceeds 15°. Adjust the page rotation in Improve page first."));
        return;
    }
    applyPagePreprocessing(settings, true, true);
}

bool MainWindow::canUndoSkewCorrection() const
{
    if (!undoStack.canUndo()) return false;
    const auto *command = dynamic_cast<const CorrectSkewCommand *>(undoStack.command(undoStack.index()-1));
    return command && command->affectedPage() == currentPage;
}

void MainWindow::restoreCurrentPage()
{
    if (rawImage.isNull() || processor.isBusy() || exportBusy || !maybeApplyEditor()) return;
    const auto *page = document.page(currentPage);
    auto settings = page ? page->preprocessing : core::PreprocessSettings{};
    const auto base = ImageProcessor::apply(rawImage, {.restoredPng = settings.restoredPng});
    if (!base) { showError(base.error()); return; }
    RestorationDialog dialog(rawImage, *base, this);
    if (dialog.exec() != QDialog::Accepted) return;
    QByteArray png;
    QBuffer buffer(&png);
    if (!buffer.open(QIODevice::WriteOnly) || !dialog.resultImage().save(&buffer, "PNG")) {
        showError(tr("Could not save the restored page."));
        return;
    }
    auto restorationGeometry=ImageProcessor::coordinatePipeline(rawImage.size(),
        {.restoredPng=settings.restoredPng,.restorationGeometry=settings.restorationGeometry});
    if(!restorationGeometry){showError(restorationGeometry.error());return;}
    restorationGeometry->steps+=dialog.geometryMapping().steps;
    settings.restorationGeometry=std::move(*restorationGeometry);
    settings.restoredPng = std::move(png);
    applyPagePreprocessing(settings, dialog.geometryChanged());
}

void MainWindow::applyPagePreprocessing(const core::PreprocessSettings &proposed, bool geometryChanged,
                                       bool recordSkewCorrection)
{
    const auto *existing = document.page(currentPage);
    const auto processed = ImageProcessor::apply(rawImage, proposed);
    if (!processed) { showError(processed.error()); return; }
    const int pdfDpi=processing::pageSourceKind(originalPath)==processing::PageSourceKind::Pdf?processor.renderedDpi(currentPage):0;
    auto coordinates=ImageProcessor::coordinatePipeline(rawImage.size(),proposed,pdfDpi);
    if(!coordinates){showError(coordinates.error());return;}
    if(coordinates->outputSize()!=QSizeF(processed->size())){showError(tr("Preprocessing coordinates do not match the prepared image."));return;}
    OcrPage page=existing?*existing:OcrPage{};
    if(existing && geometryChanged) {
        if(!page.coordinates.isDefined()) {
            auto old=ImageProcessor::coordinatePipeline(rawImage.size(),existing->preprocessing,pdfDpi);
            if(!old){showError(old.error());return;}page.coordinates=*old;
        }
        auto mapped=core::remapPageGeometry(page,*coordinates);
        if(!mapped) {showError(mapped.error());return;}
        page=std::move(*mapped);
    } else page.coordinates=*coordinates;
    if (!proposed.isIdentity() &&
        !processed->save(preparedDir.filePath(QStringLiteral("page-%1-prepared.png").arg(currentPage)), "PNG")) {
        showError(tr("Could not save the prepared page image."));
        return;
    }
    page.id = QStringLiteral("page-%1").arg(currentPage);
    page.number = currentPage;
    page.sourceWidth = rawImage.width();
    page.sourceHeight = rawImage.height();
    page.width = processed->width();
    page.height = processed->height();
    page.preprocessing = proposed;
    page.renderDpi = processing::isPagedSource(originalPath) ? processor.renderedDpi(currentPage) : 150;
    core::attachRecognitionCoordinates(page);
    if (recordSkewCorrection)
        undoStack.push(new CorrectSkewCommand(document, currentPage, std::move(page), tr("Correct skew")));
    else
        undoStack.push(new ReplacePageCommand(document, currentPage, std::move(page), tr("Improve page")));
    status->setText(tr("Improvement applied to page %1. Text, corrections, and regions were preserved.").arg(currentPage));
}

void MainWindow::runOcr()
{
    if (imagePath.isEmpty() || processor.isBusy()) return;
    if (!maybeApplyEditor()) return;
    if (const OcrPage *page = document.page(currentPage); page && !selectedRegionIds.isEmpty()) {
        QVector<ImageRegion> targets;
        for (const auto &region : page->imageRegions)
            if (selectedRegionIds.contains(region.id) && !region.notText) targets.append(region);
        if (targets.isEmpty()) {
            status->setText(tr("The selected regions are marked Not a text."));
            return;
        }
        const bool hasCorrections = std::ranges::any_of(page->words, [&](const OcrWord &word) {
            return word.reviewed && std::ranges::any_of(targets, [&](const ImageRegion &region) {
                return overlapsWord(word, region.bounds);
            });
        });
        if (hasCorrections && QMessageBox::question(this, tr("Recognize image regions"),
            tr("Some selected regions contain corrected text. New OCR may replace unmatched corrections. Continue?"))
            != QMessageBox::Yes) return;
        if (targets.size() > 1 && processor.engineId() == "tesseract" &&
            std::ranges::all_of(targets, [](const auto &r) { return !r.content.isEmpty(); })) {
            status->setText(tr("Recognizing selected blocks…"));
            processor.recognizeTypedRegions(imagePath, languageEdit->text(), currentPage, targets);
            return;
        }
        queuedImageRegions = std::move(targets);
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
    mathMode->setChecked(false);
    if (!document.nativeOcrUrl.isEmpty()) undoStack.push(new ChangeNativeOcrModelCommand(document, {}));
    processor.setEngine(std::make_unique<processing::RemoteOcrEngine>(url));
    languageEdit->setText(QStringLiteral("auto"));
    languageEdit->setReadOnly(false);
    languageEdit->setToolTip(tr("PaddleOCR-VL uses its multilingual model vocabulary."));
    languagePickerAction->setEnabled(false);
    languagePickerButton->setEnabled(true);
    QSettings(QStringLiteral("YAGF"), QStringLiteral("YAGF Revival"))
        .setValue(QStringLiteral("ocr/paddleUrl"), url.toString());
    status->setText(tr("OCR engine: PaddleOCR-VL · %1").arg(url.toString()));
    updateComparisonEngines();
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
    provider->addItem(tr("Adapted native OCR model"));
    const QStringList engineIds{QStringLiteral("tesseract"), QStringLiteral("paddleocr-remote"),
                                QStringLiteral("doctr-remote"), QStringLiteral("rapidocr-remote"),
                                QStringLiteral("surya-remote"), QStringLiteral("kraken-remote"), QStringLiteral("native-remote")};
    const auto currentEngine = static_cast<int>(engineIds.indexOf(processor.engineId()));
    provider->setCurrentIndex(qMax(0, currentEngine));
    const QString paddleUrl = settings.value(QStringLiteral("ocr/paddleUrl"),
        settings.value(QStringLiteral("ocr/remoteUrl"),
            QStringLiteral("http://127.0.0.1:8765"))).toString();
    const QString doctrUrl = settings.value(QStringLiteral("ocr/doctrUrl"),
        QStringLiteral("http://127.0.0.1:8766")).toString();
    QStringList urls{QString(), paddleUrl, doctrUrl,
        settings.value(QStringLiteral("ocr/rapidocrUrl"), QStringLiteral("http://127.0.0.1:8767")).toString(),
        settings.value(QStringLiteral("ocr/suryaUrl"), QStringLiteral("http://127.0.0.1:8768")).toString(),
        settings.value(QStringLiteral("ocr/krakenUrl"), QStringLiteral("http://127.0.0.1:8769")).toString(),
        document.nativeOcrUrl.isEmpty() ? settings.value("ocr/nativeUrl").toString() : document.nativeOcrUrl};
    const auto activeService=processor.primaryOcrService();
    if (currentEngine>0 && processing::validOcrServiceUrl(activeService.url)) urls[currentEngine]=activeService.url.toString();
    const QStringList settingKeys{QString(), QStringLiteral("ocr/paddleUrl"), QStringLiteral("ocr/doctrUrl"),
        QStringLiteral("ocr/rapidocrUrl"), QStringLiteral("ocr/suryaUrl"), QStringLiteral("ocr/krakenUrl"), QStringLiteral("ocr/nativeUrl")};
    auto *address = new QLineEdit(urls.at(provider->currentIndex()), &dialog);
    address->setPlaceholderText(tr("http://127.0.0.1:8765"));
    auto *token = new QLineEdit(&dialog);
    token->setEchoMode(QLineEdit::Password);
    token->setPlaceholderText(tr("Optional bearer token; held only for this session"));
    token->setText(activeService.token);
    connect(address,&QLineEdit::textEdited,token,&QLineEdit::clear);
    layout->addRow(tr("Engine:"), provider);
    layout->addRow(tr("Service URL:"), address);
    layout->addRow(tr("Token:"), token);
    auto *discover=new QPushButton(tr("Discover services / use several engines…"),&dialog); layout->addRow(discover);
    connect(discover,&QPushButton::clicked,&dialog,[&]{dialog.done(2);});
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(provider, &QComboBox::currentIndexChanged, &dialog, [=](int index) {
        address->setEnabled(index != 0);
        token->setEnabled(index != 0);
        address->setText(urls.at(index));
        token->clear();
    });
    address->setEnabled(provider->currentIndex() != 0);
    token->setEnabled(provider->currentIndex() != 0);
    const int result=dialog.exec();
    if (result==2) {configureOcrServices(); return;}
    if (result!=QDialog::Accepted) return;
    if (provider->currentIndex() == 0) {
        if (!document.nativeOcrUrl.isEmpty()) undoStack.push(new ChangeNativeOcrModelCommand(document, {}));
        mathMode->setChecked(false);
        processor.setEngine(std::make_unique<processing::TesseractEngine>());
        languageEdit->setText(processing::TesseractEngine::modelLanguage(languageEdit->text()));
    } else {
        const QUrl url = QUrl::fromUserInput(address->text().trimmed());
        if ((url.scheme() != QStringLiteral("http") && url.scheme() != QStringLiteral("https")) ||
            url.host().isEmpty() || !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment() ||
            !processing::validOcrServiceUrl(url)) {
            showError(tr("Enter the service base URL, for example http://127.0.0.1:8765"));
            return;
        }
        const int selected = provider->currentIndex();
        using Provider = processing::RemoteOcrEngine::Provider;
        const Provider backend = selected == 2 ? Provider::DocTr :
                                 selected == 3 ? Provider::RapidOcr :
                                 selected == 4 ? Provider::Surya :
                                 selected == 5 ? Provider::Kraken : selected == 6 ? Provider::Native : Provider::PaddleOcr;
        const bool nativeModel=selected==6 && QRegularExpression("^/models/[a-f0-9-]{36}$").match(url.path()).hasMatch();
        if (!nativeModel && !document.nativeOcrUrl.isEmpty())
            undoStack.push(new ChangeNativeOcrModelCommand(document, {}));
        mathMode->setChecked(false);
        if (nativeModel) {
            nativeOcrTokens.insert(url.adjusted(QUrl::RemovePath).toString(), token->text());
            undoStack.push(new ChangeNativeOcrModelCommand(document, url.toString()));
            // Re-entering a session token for an already selected model also
            // refreshes the provider, even though the persisted URL is unchanged.
            processor.setNativeEngine(std::make_unique<processing::RemoteOcrEngine>(url, token->text(), backend));
        } else processor.setEngine(std::make_unique<processing::RemoteOcrEngine>(url, token->text(), backend));
        settings.setValue(settingKeys.at(selected), url.toString());
        if (selected != 1) languageEdit->setText(QStringLiteral("auto"));
        else if (languageEdit->text() == QStringLiteral("eng") ||
                 languageEdit->text() == QStringLiteral("auto")) languageEdit->setText(QStringLiteral("en"));
    }
    const bool tesseract = provider->currentIndex() == 0;
    languagePickerAction->setEnabled(tesseract);
    languagePickerButton->setEnabled(!mathMode->isChecked());
    languageEdit->setReadOnly(provider->currentIndex() > 1);
    languageEdit->setToolTip(tesseract
        ? tr("Tesseract languages, for example eng+deu. The first language is primary.")
        : provider->currentIndex() == 1
            ? tr("Language configured on the PaddleOCR service, for example en")
            : tr("This engine chooses its model vocabulary; language is auto."));
    status->setText(tr("OCR engine: %1").arg(processor.engineId()));
    updateComparisonEngines();
}

void MainWindow::updatePageLayoutService()
{
    QSettings settings("YAGF","YAGF Revival");
    const processing::PageLayoutOptions options{QUrl(settings.value("layout/url","http://127.0.0.1:8771").toString()),layoutSessionToken,settings.value("layout/threshold",.5).toDouble()};
    const auto result=processor.setPageLayoutService(options,settings.value("layout/automatic",false).toBool());
    if (!result) qWarning("Could not configure AI page layout: %s",qPrintable(result.error()));
}

void MainWindow::updateComparisonEngines()
{
    if (processor.isBusy()) return;
    const auto primary=processor.primaryOcrService().key();
    std::vector<processing::ComparisonEngine> engines;
    for (const auto &service:ocrServices) if (service.enabled && service.supportsText() && service.key()!=primary && engines.size()<3) {
        auto engine=processing::makeOcrServiceEngine(service);
        if (engine) engines.push_back({service,std::move(*engine)});
    }
    processor.setComparisonEngines(std::move(engines));
}

void MainWindow::configureOcrServices()
{
    if (processor.isBusy() || exportBusy || projectLoading) return;
    auto current=processor.primaryOcrService(); current.language=languageEdit->text();
    OcrServicesDialog dialog(ocrServices,current,this,processor.formulaOcrService(),layoutSessionToken);
    if (dialog.exec()!=QDialog::Accepted) return;
    const auto formulas=dialog.formulaService();
    const auto configured=processor.setFormulaOcrService(formulas);
    if (!configured) {showError(configured.error()); return;}
    if (const auto selected=dialog.primaryService()) {
        auto engine=processing::makeOcrServiceEngine(*selected);
        if (!engine) {showError(engine.error()); return;}
        mathMode->setChecked(false);
        const bool native=selected->provider=="native" && QRegularExpression("^/models/[a-f0-9-]{36}$").match(selected->url.path()).hasMatch();
        if (native) {
            nativeOcrTokens.insert(selected->url.adjusted(QUrl::RemovePath).toString(),selected->token);
            undoStack.push(new ChangeNativeOcrModelCommand(document,selected->url.toString()));
            processor.setNativeEngine(std::move(*engine));
        } else {
            if (!document.nativeOcrUrl.isEmpty()) undoStack.push(new ChangeNativeOcrModelCommand(document,{}));
            processor.setEngine(std::move(*engine));
        }
        const bool local=selected->provider=="tesseract";
        languageEdit->setText(local ? processing::TesseractEngine::modelLanguage(selected->language.isEmpty() ? current.language : selected->language) : selected->language.isEmpty() ? QStringLiteral("auto") : selected->language);
        languageEdit->setReadOnly(!local && selected->provider!="paddleocr");
        languageEdit->setToolTip(local ? tr("Tesseract languages, for example rus+eng.") : tr("Language reported by the selected OCR service."));
        languagePickerAction->setEnabled(local);
    }
    auto primary=dialog.primaryService().value_or(current);
    if (primary.provider!="native" || !primary.url.path().startsWith("/models/"))
        QSettings("YAGF","YAGF Revival").setValue("ocr/primaryService",QJsonDocument(processing::ocrServicesJson({primary}).first().toObject()).toJson(QJsonDocument::Compact));
    QSettings("YAGF","YAGF Revival").setValue("layout/url",dialog.pageLayoutUrl());
    QSettings("YAGF","YAGF Revival").setValue("layout/threshold",dialog.pageLayoutThreshold());
    layoutSessionToken=dialog.pageLayoutToken();
    QSettings("YAGF","YAGF Revival").setValue("layout/automatic",dialog.automaticPageLayout());
    updatePageLayoutService();
    ocrServices=dialog.services();
    QSettings("YAGF","YAGF Revival").setValue("ocr/serviceLibrary",QJsonDocument(processing::ocrServicesJson(ocrServices)).toJson(QJsonDocument::Compact));
    QSettings("YAGF","YAGF Revival").setValue("ocr/formulaService",QJsonDocument(processing::ocrServicesJson({formulas}).first().toObject()).toJson(QJsonDocument::Compact));
    updateComparisonEngines(); updatePageControls(); status->setText(tr("Text and formula engines updated. Formula engine: %1.").arg(formulas.label));
}

void MainWindow::reviewOcrReadings()
{
    if (processor.isBusy() || exportBusy || imagePath.isEmpty() || !maybeApplyEditor()) return;
    const auto *page=document.page(currentPage); if (!page || (page->ocrReadings.isEmpty() && page->recognitionHistory.runs.isEmpty())) return;
    QImage image(imagePath); if (image.isNull() || image.size()!=QSize(page->width,page->height)) {showError(tr("The page image does not match the saved OCR geometry.")); return;}
    OcrReadingsDialog dialog(*page,std::move(image),this);
    if (dialog.exec()!=QDialog::Accepted || !dialog.hasChanges()) return;
    auto edited=dialog.editedPage(); core::normalizeBlockFontSizes(edited); core::setFormulaOutput(edited,document.unicodeFormulas);
    undoStack.push(new ReplacePageCommand(document,currentPage,std::move(edited),tr("Use reviewed OCR readings")));
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
    if (!processor.isBusy() && !exportBusy && languagePickerButton->isEnabled())
        languagePickerButton->showMenu();
}

void MainWindow::refreshOcrLanguageList()
{
    const QSignalBlocker blocker(ocrLanguageList);
    ocrLanguageList->clear();
    const bool local = processor.engineId() == "tesseract";
    ocrLanguageList->setVisible(local);
    ocrLanguageStatus->setText(local ? tr("Check languages to recognize together. The first code is primary.")
        : languageEdit->toolTip());
    if (!local) return;
    const auto installed = processing::installedTesseractLanguages();
    if (!installed) {
        ocrLanguageStatus->setText(tr("Could not list installed Tesseract languages. %1").arg(installed.error()));
        return;
    }
    const auto &languages = installed->codes;
    const auto selected = languageEdit->text().split('+', Qt::SkipEmptyParts);
    for (const auto &language : languages) {
        auto *item = new QListWidgetItem(language, ocrLanguageList);
        item->setData(Qt::UserRole, language);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(selected.contains(language) ? Qt::Checked : Qt::Unchecked);
    }
    if (languages.isEmpty()) ocrLanguageStatus->setText(tr("No installed OCR languages found. Choose Add language… to find a language pack."));
}

void MainWindow::runBatchOcr(bool remainingOnly)
{
    if (processor.isBusy() || exportBusy || imagePath.isEmpty() || !maybeApplyEditor()) return;
    bool hasCorrections = false;
    if (!remainingOnly) {
        for (const OcrPage &page : document.pages)
            for (const OcrWord &word : page.words) hasCorrections |= word.reviewed;
    }
    if (hasCorrections && QMessageBox::question(this, tr("Recognize all pages again"),
        tr("Matching corrections will be kept, but unmatched words may be replaced. Continue?"))
        != QMessageBox::Yes) return;
    if (!processing::isPagedSource(originalPath)) {
        status->setText(tr("Recognizing the whole page…"));
        updateRecognitionProgress(0, 1);
        processor.recognize(imagePath, languageEdit->text().trimmed(), currentPage);
        return;
    }
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

void MainWindow::updateRecognitionProgress(qsizetype completed, qsizetype total)
{
    if (!recognitionProgress) {
        recognitionProgress = new QProgressDialog(this);
        recognitionProgress->setObjectName(QStringLiteral("recognitionProgressDialog"));
        recognitionProgress->setWindowTitle(tr("Recognizing pages"));
        recognitionProgress->setCancelButtonText(tr("Cancel"));
        recognitionProgress->setMinimumDuration(0);
        recognitionProgress->setAutoClose(false);
        recognitionProgress->setAutoReset(false);
        connect(recognitionProgress, &QProgressDialog::canceled, &processor, &DocumentProcessor::cancel);
    }
    recognitionProgress->setRange(0, static_cast<int>(total));
    recognitionProgress->setLabelText(tr("%1 / %2 pages recognized").arg(completed).arg(total));
    recognitionProgress->setValue(static_cast<int>(completed));
    recognitionProgress->show();
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
    if (verification) verification->invalidate(currentPage);
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
    // Batch OCR may have used a new default DPI. Wait for its render before
    // painting new geometry over the previous preview.
    if (page && !rawImage.isNull() && (page->sourceWidth != rawImage.width() || page->sourceHeight != rawImage.height())) return;
    formattedPdfAction->setEnabled(!processor.isBusy() && !exportBusy && document.wordCount() > 0);
    nextSpecialistAction->setEnabled(!processor.isBusy() && !exportBusy && !imagePath.isEmpty() &&
        page && !page->words.isEmpty());
    ocrReadingsAction->setEnabled(!processor.isBusy() && !exportBusy && !imagePath.isEmpty() && page && (!page->ocrReadings.isEmpty() || !page->recognitionHistory.runs.isEmpty()));
    if (!page) {
        ocrButton->setToolTip(tr("Recognize the whole page"));
        selectedImageRegion = -1;
        selectedRegionIds.clear();
        view->setActiveRegionIndex(-1);
        deleteRegionButton->setEnabled(false);
        regionOcrButton->setEnabled(false);
        updateSelection({});
        layoutAction->setEnabled(false);
        pdfAction->setEnabled(false);
        pdfaAction->setEnabled(false);
        refreshImageToolbar();
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
        box->setToolTip(tr("Region %1 · %2").arg(i + 1)
            .arg(region.content == "picture" ? tr("Picture") : region.notText ? tr("Not a text") : contentLabel(region.content)));
        imageRegionBoxes.append(box);
        QString language = region.content == "picture" ? tr("Picture") : region.notText ? tr("Not a text") :
            region.language.isEmpty() ? tr("Page language") : region.language;
        if (!region.notText && !region.content.isEmpty())
            language = contentLabel(region.content) + QStringLiteral(" · ") + language;
        auto *item = new QListWidgetItem(QIcon(QPixmap::fromImage(regionPreview.copy(region.bounds)
            .scaled(regionList->iconSize(), Qt::KeepAspectRatio, Qt::SmoothTransformation))),
            tr("Block %1 · %2\n%3 × %4 px").arg(i + 1).arg(language)
                .arg(region.bounds.width()).arg(region.bounds.height()), regionList);
        item->setData(Qt::UserRole, region.id);
        item->setSizeHint(regionList->gridSize());
        item->setTextAlignment(Qt::AlignHCenter);
        item->setToolTip(tr("Block %1 · %2\nRight-click to change its content type or language.")
            .arg(i + 1).arg(language));
        if (!region.detectedLabel.isEmpty())
            item->setToolTip(item->toolTip() + tr("\nDetected: %1 · model score %2%")
                .arg(region.detectedLabel).arg(region.detectionScore * 100, 0, 'f', 1));
        if (region.notText) item->setForeground(QColor(110, 110, 110));
    }
    recognizeAction->setText(mathMode->isChecked() ? tr("Recognize math") : tr("Recognize"));
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
    selectedRegionIds.clear();
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
        edited.renderDpi = processing::isPagedSource(originalPath) ? processor.renderedDpi(currentPage) : 150;
    }
    edited.imageRegions.append({QUuid::createUuid().toString(QUuid::WithoutBraces), area});
    selectedRegionIds = {edited.imageRegions.last().id};
    selectedImageRegion = static_cast<int>(edited.imageRegions.size()) - 1;
    undoStack.push(new ReplacePageCommand(document, currentPage, std::move(edited), tr("Add image region")));
}

void MainWindow::refreshImageRegionSelection(bool updateWords)
{
    const OcrPage *page = document.page(currentPage);
    QSet<QString> present;
    QVector<int> rows;
    for (int i = 0; page && i < page->imageRegions.size(); ++i) {
        const auto &id = page->imageRegions[i].id;
        if (selectedRegionIds.contains(id)) {
            present.insert(id);
            rows.append(i);
        }
    }
    selectedRegionIds = std::move(present);
    if (!rows.contains(selectedImageRegion)) selectedImageRegion = rows.value(0, -1);
    view->setActiveRegionIndex(selectedImageRegion);
    {
        const QSignalBlocker blocker(regionList);
        regionList->setCurrentRow(selectedImageRegion, QItemSelectionModel::NoUpdate);
        for (int i = 0; i < regionList->count(); ++i)
            regionList->item(i)->setSelected(selectedRegionIds.contains(regionList->item(i)->data(Qt::UserRole).toString()));
    }
    for (int i = 0; i < imageRegionBoxes.size(); ++i) {
        const bool active = selectedRegionIds.contains(page->imageRegions[i].id);
        const bool notText = page->imageRegions[i].notText;
        const auto &kind = page->imageRegions[i].content;
        QColor color = kind == "picture" ? QColor(174, 109, 46) : notText ? QColor(110, 110, 110) : kind == "math" ? QColor(49, 93, 172) :
            kind == "mixed" ? QColor(127, 72, 153) : QColor(25, 135, 55);
        if (active) color = QColor(210, 65, 25);
        imageRegionBoxes[i]->setPen(QPen(color, active ? 3 : 2, notText ? Qt::DashLine : Qt::SolidLine));
        color.setAlpha(active ? 45 : 30);
        imageRegionBoxes[i]->setBrush(color);
    }
    deleteRegionButton->setEnabled(selectedImageRegion >= 0 && !processor.isBusy() && !exportBusy);
    regionOcrButton->setEnabled(rows.size() == 1 && !page->imageRegions[selectedImageRegion].notText &&
                                !processor.isBusy() && !exportBusy);
    QVector<int> matches;
    for (int i = 0; i < displayedWords.size(); ++i)
        if (rows.isEmpty() || std::ranges::any_of(rows, [&](int row) {
            return overlapsWord(*displayedWords.at(i), page->imageRegions[row].bounds);
        })) matches.append(i);
    const QString scope = rows.isEmpty() ? tr("Recognize the whole page")
                                         : tr("Recognize selected regions (%1)").arg(rows.size());
    ocrButton->setToolTip(scope);
    if (!rows.isEmpty()) status->setText(tr("%1 regions selected · %2 words").arg(rows.size()).arg(matches.size()));
    else status->setText(tr("Recognize will process the whole page."));
    if (updateWords) updateSelection(matches);
    refreshImageToolbar();
}

void MainWindow::refreshImageToolbar()
{
    if (!rotateLeftAction) return;
    const bool selected = !selectedRegionIds.isEmpty();
    const bool ready = !processor.isBusy() && !exportBusy && !projectLoading && !imagePath.isEmpty();
    rotateLeftAction->setVisible(!selected);
    rotateRightAction->setVisible(!selected);
    correctSkewAction->setVisible(!selected);
    rotateLeftAction->setEnabled(ready);
    rotateRightAction->setEnabled(ready);
    correctSkewAction->setEnabled(ready);
    const bool undoSkew = canUndoSkewCorrection();
    undoSkewAction->setVisible(!selected && undoSkew);
    undoSkewAction->setEnabled(ready && undoSkew);
    imageSplitAction->setEnabled(ready);
    aiPageAreasAction->setEnabled(ready);
    const auto *page = document.page(currentPage);
    aiSplitBlockAction->setEnabled(ready && page && selectedRegionIds.size()==1);
    refineFormulaAction->setEnabled(ready && page && std::ranges::any_of(page->imageRegions, [](const ImageRegion &r) {
        return !r.notText && r.content == "math";
    }));
    imageClearBlocksAction->setEnabled(ready && page && !page->imageRegions.isEmpty());
    for (auto *action : {imageRecognizeBlockAction, imageBlockOptionsAction, imageRemoveBlockAction}) {
        action->setVisible(selected);
        action->setEnabled(ready && selected);
    }
    imageBlockOptionsAction->setEnabled(ready && selectedRegionIds.size() == 1);
    bool text = false;
    if (page) for (const auto &region : page->imageRegions)
        text |= selectedRegionIds.contains(region.id) && !region.notText;
    imageRecognizeBlockAction->setEnabled(ready && text);
}

void MainWindow::activateImageRegion(int index, bool toggle)
{
    if (!maybeApplyEditor()) return;
    const auto *page = document.page(currentPage);
    if (!page || index < -1 || index >= page->imageRegions.size()) return;
    if (!toggle) selectedRegionIds.clear();
    if (index >= 0) {
        const auto &id = page->imageRegions[index].id;
        if (toggle && selectedRegionIds.contains(id)) selectedRegionIds.remove(id);
        else selectedRegionIds.insert(id);
    }
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
    edited.imageRegions[index].autoDetected = false;
    edited.imageRegions[index].detectedLabel.clear();
    edited.imageRegions[index].detectionScore = -1;
    selectedImageRegion = index;
    undoStack.push(new ReplacePageCommand(document, currentPage, std::move(edited), tr("Move image region")));
}

void MainWindow::deleteImageRegion(int index)
{
    const OcrPage *page = document.page(currentPage);
    if (!page || index < 0 || index >= page->imageRegions.size() || processor.isBusy() ||
        exportBusy || !maybeApplyEditor()) return;
    OcrPage edited = *page;
    selectedRegionIds.remove(edited.imageRegions.at(index).id);
    edited.imageRegions.removeAt(index);
    selectedImageRegion = -1;
    undoStack.push(new ReplacePageCommand(document, currentPage, std::move(edited), tr("Delete image region")));
}

void MainWindow::clearPageBlocks(int pageNumber)
{
    if (processor.isBusy() || exportBusy || !maybeApplyEditor()) return;
    const auto *page = document.page(pageNumber);
    if (!page || page->imageRegions.isEmpty()) return;
    OcrPage edited = *page;
    edited.imageRegions.clear();
    if (pageNumber == currentPage) {
        selectedRegionIds.clear();
        selectedImageRegion = -1;
    }
    undoStack.push(new ReplacePageCommand(document, pageNumber, std::move(edited), tr("Clear all blocks")));
    statusBar()->showMessage(tr("Cleared all blocks on page %1").arg(pageNumber), 5000);
}

void MainWindow::showPageContextMenu(const QPoint &position)
{
    const auto *item = pageList->itemAt(position);
    if (!item) return;
    const int number = pageList->row(item)+1;
    showPageActions(number, pageList->viewport()->mapToGlobal(position));
}

void MainWindow::showPageActions(int number, const QPoint &globalPosition)
{
    const auto *page = document.page(number);
    QMenu menu(this);
    menu.setObjectName(QStringLiteral("pageContextMenu"));
    int selected=-1;
    if (number==currentPage && page && selectedRegionIds.size()==1)
        for (int i=0;i<page->imageRegions.size();++i) if (selectedRegionIds.contains(page->imageRegions[i].id)) { selected=i; break; }
    if (selected>=0) {
        auto *copy=menu.addAction(tr("Copy selected block as image"));
        copy->setObjectName("copyBlockImageAction");
        copy->setEnabled(!processor.isBusy() && !exportBusy && !projectLoading && !imagePath.isEmpty());
        connect(copy,&QAction::triggered,this,[this,index=selected] { copyBlockImage(index); });
    }
    auto *clear = menu.addAction(QIcon(":/actions/clear-blocks.png"), tr("Clear all blocks"));
    clear->setObjectName(QStringLiteral("clearPageBlocks"));
    clear->setEnabled(!processor.isBusy() && !exportBusy && page && !page->imageRegions.isEmpty());
    connect(clear, &QAction::triggered, this, [this, number] { clearPageBlocks(number); });
    menu.exec(globalPosition);
}

QString MainWindow::blockLanguage(const ImageRegion &region) const
{
    return region.language.isEmpty() ? languageEdit->text().trimmed() : region.language;
}

void MainWindow::trainNativeOcr()
{
    if (processor.isBusy() || exportBusy || projectLoading || originalPath.isEmpty() || !maybeApplyEditor()) return;
    NativeTrainingDialog dialog(document, currentPage, this);
    if (dialog.exec() != QDialog::Accepted || dialog.selectedModelUrl().isEmpty()) return;
    mathMode->setChecked(false);
    nativeOcrTokens.insert(dialog.selectedModelUrl().adjusted(QUrl::RemovePath).toString(), dialog.sessionToken());
    undoStack.push(new ChangeNativeOcrModelCommand(document, dialog.selectedModelUrl().toString()));
    status->setText(tr("Validated native OCR model selected. Recognize a page to use it. Undo restores the previous model."));
}

void MainWindow::syncNativeOcrModel()
{
    if (appliedNativeOcrUrl == document.nativeOcrUrl || processor.isBusy()) return;
    appliedNativeOcrUrl = document.nativeOcrUrl;
    if (appliedNativeOcrUrl.isEmpty()) {
        processor.setNativeEngine({});
        const auto engine = processor.engineId();
        languageEdit->setText(engine == "tesseract" ? processing::TesseractEngine::modelLanguage(document.language)
            : engine == "paddleocr-remote" ? QStringLiteral("en") : QStringLiteral("auto"));
        languageEdit->setReadOnly(engine != "tesseract" && engine != "paddleocr-remote");
    } else {
        processor.setNativeEngine(std::make_unique<processing::RemoteOcrEngine>(QUrl(appliedNativeOcrUrl), nativeOcrTokens.value(QUrl(appliedNativeOcrUrl).adjusted(QUrl::RemovePath).toString()),
            processing::RemoteOcrEngine::Provider::Native));
        languageEdit->setText("auto"); languageEdit->setReadOnly(true);
    }
    updateComparisonEngines();
}

QImage MainWindow::displayedPageImage() const
{
    for (auto *item : scene->items()) if (auto *pixmap = qgraphicsitem_cast<QGraphicsPixmapItem *>(item)) {
        return pixmap->pixmap().toImage();
    }
    return {};
}

void MainWindow::classifyPageAreas()
{
    if (processor.isBusy() || exportBusy || projectLoading || imagePath.isEmpty() || !maybeApplyEditor()) return;
    const QImage image=displayedPageImage();
    if (image.isNull()) { showError(tr("Could not read the page image for layout analysis.")); return; }
    const auto *old = document.page(currentPage);
    AiPageLayoutDialog dialog(image, old ? old->imageRegions : QVector<ImageRegion>{}, this, currentPage, layoutSessionToken);
    processing::OcrOptions context;context.pageNumber=currentPage;
    auto mapping=ImageProcessor::coordinatePipeline(rawImage.size(),displayedPreprocessing,
        processing::pageSourceKind(originalPath)==processing::PageSourceKind::Pdf?processor.renderedDpi(currentPage):0);
    if(mapping)context.historyCoordinates=*mapping;
    dialog.enableHistory(context,originalPath,core::preprocessingEvidence(displayedPreprocessing,processor.renderedDpi(currentPage)));
    const int outcome=dialog.exec(); layoutSessionToken=dialog.sessionToken();updatePageLayoutService();
    const auto history=dialog.recordedHistory();bool historyStored=false;
    auto preserve=qScopeGuard([&] {
        if(historyStored || history.runs.isEmpty())return;
        auto page=document.page(currentPage)?*document.page(currentPage):OcrPage{};
        if(!document.page(currentPage)){page.id=QStringLiteral("page-%1").arg(currentPage);page.number=currentPage;
            page.width=image.width();page.height=image.height();page.sourceWidth=rawImage.width();page.sourceHeight=rawImage.height();
            page.preprocessing=displayedPreprocessing;if(mapping)page.coordinates=*mapping;}
        core::mergeRecognitionHistory(page.recognitionHistory,history);
        undoStack.push(new ReplacePageCommand(document,currentPage,std::move(page),tr("Record layout recognition history")));
    });
    if (outcome != QDialog::Accepted) return;
    OcrPage page = old ? *old : OcrPage{};
    if (!old) {
        page.number = currentPage;
        page.id = QStringLiteral("page-%1").arg(currentPage);
        page.width = image.width(); page.height = image.height();
        page.sourceWidth = rawImage.width(); page.sourceHeight = rawImage.height();
        page.preprocessing = displayedPreprocessing;
        page.renderDpi = processor.renderedDpi(currentPage);
    }
    page.language = languageEdit->text();
    page.imageRegions = dialog.regions();
    core::mergeRecognitionHistory(page.recognitionHistory,history);historyStored=true;
    selectedRegionIds.clear(); selectedImageRegion = -1;
    undoStack.push(new ReplacePageCommand(document, currentPage, std::move(page), tr("AI page areas")));
    status->setText(tr("Page areas updated. Review the blocks, then Recognize to refresh the OCR text."));
}

void MainWindow::refineFormulaBlocks()
{
    if (processor.isBusy() || exportBusy || projectLoading || !maybeApplyEditor()) return;
    const auto *page = document.page(currentPage);
    if (!page) return;
    const QImage image = displayedPageImage();
    if (image.isNull()) { showError(tr("Could not read the page image for formula refinement.")); return; }

    std::stop_source cancellation;
    QProgressDialog progress(tr("Looking for symbols near formula blocks…"), tr("Cancel"), 0, 0, this);
    progress.setWindowModality(Qt::WindowModal);
    QFutureWatcher<QVector<ImageRegion>> watcher;
    connect(&watcher, &QFutureWatcherBase::finished, &progress, &QDialog::accept);
    connect(&progress, &QProgressDialog::canceled, this, [&] { cancellation.request_stop(); });
    watcher.setFuture(QtConcurrent::run(processing::growFormulaBlocks, image, page->imageRegions, cancellation.get_token()));
    if (progress.exec() != QDialog::Accepted) cancellation.request_stop();
    watcher.waitForFinished();
    if (cancellation.stop_requested()) return;

    auto regions = watcher.result();
    int changed = 0;
    for (const auto &r : regions) {
        if (r.content == "math" && std::ranges::any_of(page->imageRegions, [&](const auto &old) {
                return old.id == r.id && old.bounds != r.bounds;
            })) ++changed;
    }
    if (!changed) {
        status->setText(tr("No nearby symbols could be assigned to a formula block."));
        return;
    }
    OcrPage edited = *page;
    edited.imageRegions = std::move(regions);
    selectedRegionIds.clear();
    selectedImageRegion = -1;
    undoStack.push(new ReplacePageCommand(document, currentPage, std::move(edited), tr("Refine formula blocks")));
    status->setText(tr("Expanded %1 formula block(s). Review the outlines, then recognize the page.").arg(changed));
}

void MainWindow::splitSelectedBlockWithAi()
{
    if(processor.isBusy() || exportBusy || projectLoading || imagePath.isEmpty() || selectedRegionIds.size()!=1 || !maybeApplyEditor())return;
    const auto *page=document.page(currentPage);if(!page)return;
    const auto parent=std::ranges::find_if(page->imageRegions,[&](const auto &r){return selectedRegionIds.contains(r.id);});
    if(parent==page->imageRegions.end())return;
    const ImageRegion original=*parent;const QImage image=displayedPageImage();
    if(image.isNull() || !image.rect().contains(original.bounds)){showError(tr("The selected block lies outside the displayed page."));return;}
    AiPageLayoutDialog dialog(image.copy(original.bounds),{},this,currentPage,layoutSessionToken,true);
    processing::OcrOptions context;context.pageNumber=currentPage;context.historyCoordinates=page->coordinates;
    if(!context.historyCoordinates.isDefined()) {
        auto mapping=ImageProcessor::coordinatePipeline(rawImage.size(),displayedPreprocessing,
            processing::pageSourceKind(originalPath)==processing::PageSourceKind::Pdf?processor.renderedDpi(currentPage):0);
        if(mapping)context.historyCoordinates=*mapping;
    }
    context=processing::croppedOcrOptions(context,original.bounds);
    dialog.enableHistory(context,originalPath,core::preprocessingEvidence(displayedPreprocessing,processor.renderedDpi(currentPage)));
    const int outcome=dialog.exec();layoutSessionToken=dialog.sessionToken();updatePageLayoutService();
    const auto history=dialog.recordedHistory();bool historyStored=false;
    auto preserve=qScopeGuard([&] {
        const auto *saved=document.page(currentPage);if(historyStored || history.runs.isEmpty() || !saved)return;
        auto revised=*saved;core::mergeRecognitionHistory(revised.recognitionHistory,history);
        undoStack.push(new ReplacePageCommand(document,currentPage,std::move(revised),tr("Record layout recognition history")));
    });
    if(outcome!=QDialog::Accepted)return;
    auto subdivisions=dialog.regions();if(subdivisions.size()<2)return;
    page=document.page(currentPage);if(!page)return;
    const auto found=std::ranges::find_if(page->imageRegions,[&](const auto &r){return r.id==original.id && r.bounds==original.bounds;});
    if(found==page->imageRegions.end() || page->imageRegions.size()-1+subdivisions.size()>1000)return;
    OcrPage edited=*page;core::mergeRecognitionHistory(edited.recognitionHistory,history);
    const int index=found-page->imageRegions.begin();edited.imageRegions.removeAt(index);
    for(int i=0;i<subdivisions.size();++i) {
        auto &region=subdivisions[i];region.bounds.translate(original.bounds.topLeft());
        if(!original.bounds.contains(region.bounds))return;
        if(region.language.isEmpty())region.language=original.language;
        if(std::ranges::any_of(edited.imageRegions,[&](const auto &r){return r.bounds.intersects(region.bounds);}))return;
        edited.imageRegions.insert(index+i,region);
    }
    selectedRegionIds.clear();selectedImageRegion=-1;
    historyStored=true;
    undoStack.push(new ReplacePageCommand(document,currentPage,std::move(edited),tr("AI split block")));
    status->setText(tr("Selected block replaced with %1 subdivisions. Review their types, then Recognize to refresh the text.").arg(subdivisions.size()));
}

void MainWindow::setBlockLanguage(int index, const QString &language, bool notText)
{
    const auto *page = document.page(currentPage);
    if (!page || index < 0 || index >= page->imageRegions.size() || processor.isBusy() ||
        exportBusy || !maybeApplyEditor()) return;
    OcrPage edited = *document.page(currentPage);
    auto &region = edited.imageRegions[index];
    if (region.language == language && region.notText == notText) return;
    region.autoDetected = false;
    region.detectedLabel.clear();
    region.detectionScore = -1;
    if (!notText && region.content == "picture") region.content = "text";
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
    connect(recognize, &QAction::triggered, this, &MainWindow::runOcr);
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
    const bool tesseract = !region.content.isEmpty() || processor.engineId() == QStringLiteral("tesseract");
    const QStringList labels{tr("Automatic"), tr("English"), tr("German"), tr("French"), tr("Spanish"),
        tr("Italian"), tr("Portuguese"), tr("Russian"), tr("Ukrainian"), tr("Chinese"), tr("Japanese"),
        tr("Korean"), tr("Arabic"), tr("Hebrew")};
    const QStringList codes = tesseract
        ? QStringList{"", "eng", "deu", "fra", "spa", "ita", "por", "rus", "ukr", "chi_sim", "jpn", "kor", "ara", "heb"}
        : QStringList{"auto", "en", "de", "fr", "es", "it", "pt", "ru", "uk", "zh", "ja", "ko", "ar", "he"};
    for (int i = tesseract ? 1 : 0; i < labels.size(); ++i) addLanguage(labels[i], codes[i]);
    if (processor.engineId() == "tesseract") {
        languages->addAction(tr("Add language…"), this, [this, index, region] {
            if (const auto codes = chooseAdditionalOcrLanguage(this, region.language))
                setBlockLanguage(index, *codes, false);
        });
    }
    auto *other = languages->addAction(tr("Other language…"));
    connect(other, &QAction::triggered, this, [this, index, region] {
        bool ok = false;
        const QString code = QInputDialog::getText(this, tr("Select block language"),
            tr("OCR language code or combination, for example eng+deu:"), QLineEdit::Normal, region.language, &ok).trimmed();
        if (ok && QRegularExpression(QStringLiteral("^[A-Za-z0-9_+/-]{1,64}$")).match(code).hasMatch())
            setBlockLanguage(index, code, false);
    });
    auto *contents = menu.addMenu(tr("Block content"));
    for (const auto &kind : QStringList{"", "text", "math", "mixed", "picture"}) {
        auto *action = contents->addAction(contentLabel(kind));
        action->setCheckable(true);
        action->setChecked(region.content == kind && (kind == "picture" || !region.notText));
        connect(action, &QAction::triggered, this, [this, index, kind] {
            OcrPage changed = *document.page(currentPage);
            changed.imageRegions[index].autoDetected = false;
            changed.imageRegions[index].content = kind;
            changed.imageRegions[index].notText = kind == "picture";
            changed.imageRegions[index].detectedLabel.clear();
            changed.imageRegions[index].detectionScore = -1;
            undoStack.push(new ReplacePageCommand(document, currentPage, std::move(changed), tr("Set block content")));
        });
    }
    menu.addAction(aiSplitBlockAction);
    menu.addAction(refineFormulaAction);
    auto *copy=menu.addAction(QIcon::fromTheme("edit-copy",style()->standardIcon(QStyle::SP_FileIcon)),tr("Copy block as image"));
    copy->setObjectName("copyBlockImageAction");
    copy->setIconVisibleInMenu(true);
    connect(copy,&QAction::triggered,this,[this,index] { copyBlockImage(index); });
    auto *clear = menu.addAction(QIcon(":/actions/clear-blocks.png"), tr("Clear all blocks"));
    clear->setObjectName("clearAllBlocksAction");
    connect(clear, &QAction::triggered, this, [this] { clearPageBlocks(currentPage); });
    menu.addSeparator();
    auto *remove = menu.addAction(removeBlockIcon(), tr("Remove block"));
    remove->setIconVisibleInMenu(true);
    connect(remove, &QAction::triggered, this, [this, index] { deleteImageRegion(index); });
    menu.exec(globalPosition);
}

void MainWindow::copyBlockImage(int index)
{
    if (processor.isBusy() || exportBusy || projectLoading || imagePath.isEmpty()) return;
    const auto *page=document.page(currentPage);
    if (!page || index<0 || index>=page->imageRegions.size()) return;
    QImage image;
    for (auto *item:scene->items()) if (auto *pixmap=qgraphicsitem_cast<QGraphicsPixmapItem *>(item)) { image=pixmap->pixmap().toImage(); break; }
    const auto bounds=page->imageRegions[index].bounds;
    if (image.isNull() || bounds.isEmpty() || !image.rect().contains(bounds)) { showError(tr("Could not read the block's source image.")); return; }
    QApplication::clipboard()->setImage(image.copy(bounds));
    statusBar()->showMessage(tr("Copied block image (%1 × %2 pixels).").arg(bounds.width()).arg(bounds.height()),5000);
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
        if (page && !selectedRegionIds.isEmpty()) {
            for (const auto &region : page->imageRegions)
                if (selectedRegionIds.contains(region.id) && !region.notText) languages.append(blockLanguage(region));
            languages.removeDuplicates();
            if (languages.isEmpty()) languages.append(fallback);
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
    spellCheckAction->setToolTip(spellCheckAction->isChecked() && !spellHighlighter->activeDictionaries().isEmpty()
        ? tr("Right-click an underlined word for replacements.") : spellHighlighter->statusText());
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
    editorWordRanges.clear();
    selectedWords = indices;
    const auto *page = document.page(currentPage);
    editFormulaAction->setEnabled(!processor.isBusy() && !exportBusy && page &&
        !processing::detail::editableFormulaSpans(*page, indices).isEmpty());
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
    editorSections.clear();
    selectionEditor->clear();
    editorPending = false;
    const bool structured = std::ranges::any_of(indices,
        [this](int i) { return displayedWords.at(i)->layoutBlock; });
    selectionEditor->setReadOnly(indices.isEmpty());
    applyButton->setEnabled(!processor.isBusy() && !exportBusy && !indices.isEmpty());
    selectionLabel->setText(indices.isEmpty() ? tr("Recognized text") : tr("Selected text (%1 items)").arg(indices.size()));
    QTextCursor cursor(selectionEditor->document());
    cursor.movePosition(QTextCursor::Start);
    const OcrWord *previous = nullptr;
    for (int index : indices) {
        if (index < 0 || index >= displayedWords.size()) continue;
        const OcrWord *word = displayedWords.at(index);
        if (previous) {
            const bool paragraph = previous->block != word->block || previous->paragraph != word->paragraph;
            const auto standalone = [](const OcrWord &item) {
                return item.layoutBlock && !(item.text.startsWith("\\(") && item.text.endsWith("\\)"));
            };
            const bool separate = index != editorSections.last().lastWord + 1 ||
                (structured && (paragraph || standalone(*word) || standalone(*previous)));
            const QString separator = separate || paragraph
                ? QStringLiteral("\n\n") : previous->line != word->line ? QStringLiteral("\n") : QStringLiteral(" ");
            cursor.insertText(separator);
            if (separate) {
                auto boundary = cursor;
                boundary.movePosition(QTextCursor::PreviousCharacter);
                editorSections.append({index, index, boundary});
            } else editorSections.last().lastWord = index;
        } else editorSections.append({index, index, {}});
        QTextCharFormat format;
        if (!word->fontFamily.isEmpty()) format.setFontFamilies({word->fontFamily});
        format.setFontWeight(word->bold ? QFont::Bold : QFont::Normal);
        format.setFontItalic(word->italic);
        if (word->fontSizePt > 0) format.setFontPointSize(word->fontSizePt);
        editorWordRanges.insert(index, {cursor.position(), int(word->text.size())});
        cursor.insertText(word->text, format);
        previous = word;
    }
    selectionEditor->moveCursor(QTextCursor::Start);
    teachSymbolAction->setEnabled(!processor.isBusy() && !exportBusy && !projectLoading && !imagePath.isEmpty() && !indices.isEmpty());
    refreshSpellLanguage();
}

void MainWindow::editSelectedFormula()
{
    if (processor.isBusy() || exportBusy || !maybeApplyEditor()) return;
    const auto *page = document.page(currentPage);
    if (!page) return;
    const auto spans = processing::detail::editableFormulaSpans(*page, selectedWords);
    if (spans.isEmpty()) return;
    auto span = spans.first();
    if (spans.size() > 1) {
        QStringList choices;
        for (const auto &item : spans)
            choices.append(tr("%1. %2").arg(choices.size() + 1)
                .arg(core::formulaSourceText(page->words[item.word]).mid(item.start, item.length).simplified().left(100)));
        bool accepted = false;
        const QString chosen = QInputDialog::getItem(this, tr("Edit formula"), tr("Choose an equation"),
                                                      choices, 0, false, &accepted);
        if (!accepted) return;
        span = spans.at(choices.indexOf(chosen));
    }
    const auto source = core::formulaSourceText(page->words[span.word]).mid(span.start, span.length);
    const auto replacement = editFormulaSource(this, source, span.display);
    if (!replacement || *replacement == source) return;
    auto edited = *page;
    auto &word = edited.words[span.word];
    word.text=core::formulaSourceText(word);
    word.latexText.clear();
    word.text.replace(span.start, span.length, *replacement);
    word.reviewed = true;
    word.readingEngine.clear();
    word.alternative = {};
    core::setFormulaOutput(edited,document.unicodeFormulas);
    undoStack.push(new ReplaceTextCommand(document, currentPage, std::move(edited)));
    updateSelection({span.word});
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
    if (editorSections.isEmpty()) { editorPending = false; return true; }
    const QString plain = selectionEditor->toPlainText();
    const auto *page = document.page(currentPage);
    if (!page) return false;

    // Keep each structured block in its own source box. A live cursor in each
    // separator also distinguishes a block boundary from blank lines inside a table.
    QVector<int> starts{0};
    for (qsizetype i = 1; i < editorSections.size() && !plain.isEmpty(); ++i) {
        const int pos = editorSections[i].separator.position();
        if (pos > starts.last() && plain.mid(pos - 1, 2) == "\n\n") {
            starts.append(pos + 1);
            continue;
        }
        // Replacing the entire editor contents collapses the live cursors.
        starts = {0};
        for (qsizetype at = plain.indexOf("\n\n"); at >= 0; at = plain.indexOf("\n\n", at + 2))
            starts.append(at + 2);
        if (starts.size() != editorSections.size()) {
            showError(tr("Keep a blank line between the selected OCR blocks, or select a single block to change its paragraph breaks."));
            return false;
        }
        break;
    }

    static const QRegularExpression tokenPattern(QStringLiteral(R"(\\\([\s\S]*?\\\)|\S+)"));
    const auto editedWord = [&](int at, const QString &text, bool layoutBlock) {
        QTextCursor cursor(selectionEditor->document());
        cursor.setPosition(at);
        cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
        const auto format = cursor.charFormat();
        return core::EditedWord{text, format.fontWeight() >= QFont::Bold, format.fontItalic(),
            format.fontPointSize() > 0 ? qRound(format.fontPointSize()) : 0,
            format.fontFamilies().toStringList().value(0), layoutBlock};
    };

    auto updated = *page;
    QStringList selectedIds;
    int replacementCount = 0;
    // Later edits cannot shift the source indices of earlier sections.
    for (qsizetype i = editorSections.size(); i-- > 0;) {
        const auto &section = editorSections[i];
        const int start = plain.isEmpty() ? 0 : starts[i];
        const int end = !plain.isEmpty() && i + 1 < starts.size() ? starts[i + 1] - 2 : plain.size();
        const QString text = plain.mid(start, end - start);
        QVector<core::EditedWord> tokens;
        if (section.firstWord == section.lastWord && (page->words[section.firstWord].layoutBlock ||
            (document.unicodeFormulas && !page->words[section.firstWord].latexText.isEmpty()))) {
            if (!text.trimmed().isEmpty()) tokens.append(editedWord(start, text, page->words[section.firstWord].layoutBlock));
        } else {
            auto matches = tokenPattern.globalMatch(text);
            while (matches.hasNext()) {
                const auto match = matches.next();
                const auto token = match.captured();
                tokens.append(editedWord(start + match.capturedStart(), token,
                    token.startsWith("\\(") && token.endsWith("\\)")));
            }
        }
        replacementCount += tokens.size();
        QVector<int> selection;
        bool changed = tokens.size() != section.lastWord - section.firstWord + 1;
        for (int index = section.firstWord; index <= section.lastWord; ++index) {
            selection.append(index);
            if (changed) continue;
            const auto &old = page->words[index];
            const auto &edit = tokens[index - section.firstWord];
            changed = old.text != edit.text || old.bold != edit.bold || old.italic != edit.italic ||
                (edit.fontSizePt > 0 && old.fontSizePt != edit.fontSizePt) ||
                (!edit.fontFamily.isEmpty() && old.fontFamily != edit.fontFamily);
        }
        if (changed || editorSections.size() == 1) {
            auto result = core::WordEditor::apply(updated, selection, tokens);
            if (!result) { showError(result.error()); return false; }
            updated = std::move(*result);
        }
        for (qsizetype j = 0; j < tokens.size(); ++j) selectedIds.append(updated.words[section.firstWord + j].id);
    }
    editorPending = false;
    core::setFormulaOutput(updated,document.unicodeFormulas);
    if (selectedWords.size() == 1 && replacementCount > 1)
        undoStack.push(new SplitWordCommand(document, currentPage, std::move(updated)));
    else if (selectedWords.size() > 1 && replacementCount == 1)
        undoStack.push(new MergeWordsCommand(document, currentPage, std::move(updated)));
    else undoStack.push(new ReplaceTextCommand(document, currentPage, std::move(updated)));
    updateSelection(indicesForIds(selectedIds));
    statusBar()->showMessage(tr("Correction applied. Use Edit → Undo to reverse it."));
    return true;
}

bool MainWindow::maybeApplyEditor()
{
    if (!editorPending) return true;
    return applyCorrection();
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
    file.write(QJsonDocument(document.toJson(false)).toJson(QJsonDocument::Indented));
}

void MainWindow::exportLatex()
{
    if (processor.isBusy() || exportBusy || projectLoading || document.pages.isEmpty() || !maybeApplyEditor()) return;
    if (document.pages.size()<pageCount && QMessageBox::question(this,tr("Unrecognized pages"),
        tr("Export only the recognized pages?"),QMessageBox::Yes|QMessageBox::Cancel,QMessageBox::Cancel)!=QMessageBox::Yes) return;
    chooseExportPath(tr("Export LaTeX document"),tr("LaTeX document (*.tex)"),"tex",ExportFormat::LatexDocument);
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
    const bool pictures = std::ranges::any_of(document.pages, [](const auto &page) {
        return std::ranges::any_of(page.imageRegions, [](const auto &block) { return block.content == "picture" && block.notText; });
    });
    if (!document.wordCount() && !pictures) { showError(tr("Recognize the document before exporting its reading order.")); return; }
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
    ExportSaveDialog dialog(this, title, {}, filter);
    dialog.imageExport = format == ExportFormat::ScanImage;
    const bool formatted = format == ExportFormat::FormattedPdf;
    if (dialog.imageExport || formatted) dialog.setOption(QFileDialog::DontUseNativeDialog);
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    if (!dialog.imageExport) dialog.setDefaultSuffix(suffix);
    QCheckBox *typeset = nullptr;
    QSettings settings(QStringLiteral("YAGF"), QStringLiteral("YAGF Revival"));
    const QString typesetKey = QStringLiteral("export/typesetFormulas");
    if (formatted) {
        const bool available = processing::PdfExporter::hasPdfLatex();
        typeset = new QCheckBox(available ? tr("Typeset math formulas") :
            tr("Typeset math formulas (requires pdfLaTeX)"), &dialog);
        typeset->setObjectName(QStringLiteral("typesetFormulas"));
        typeset->setToolTip(tr("Use LaTeX to draw equations. Leave unchecked to keep formulas as scanned."));
        typeset->setEnabled(available);
        typeset->setChecked(available && settings.value(typesetKey, false).toBool());
        if (auto *grid = qobject_cast<QGridLayout *>(dialog.layout()))
            grid->addWidget(typeset, grid->rowCount(), 1);
        else dialog.layout()->addWidget(typeset);
    }
    if (dialog.exec() != QDialog::Accepted) return;
    const QString path = dialog.selectedFiles().value(0);
    if (path.isEmpty()) return;
    if (QFileInfo(path) == QFileInfo(originalPath) || (!projectPath.isEmpty() && QFileInfo(path) == QFileInfo(projectPath))) {
        showError(tr("Choose a destination separate from the source and project files."));
        return;
    }
    const bool typesetFormulas = typeset && typeset->isChecked();
    if (typeset && typeset->isEnabled()) settings.setValue(typesetKey, typesetFormulas);
    startExport(path, format, typesetFormulas);
}

void MainWindow::startExport(const QString &path, ExportFormat format, bool typesetFormulas)
{
    exportPath = path;
    exportCancel = std::make_shared<std::atomic_bool>(false);
    exportBusy = true;
    setBusy(true);
    status->setText(tr("Preparing export…"));
    const OcrDocument snapshot = document;
    const auto cancel = exportCancel;
    exportWatcher.setFuture(QtConcurrent::run([this, snapshot, path, format, typesetFormulas, cancel, pageNumber = currentPage] {
        const auto progress = [this](qsizetype completed, qsizetype total) {
            emit exportProgress(completed, total);
        };
        if (format == ExportFormat::ScanImage)
            return processing::writeScanPage(snapshot, pageNumber, path,
                QFileInfo(path).suffix().compare("png", Qt::CaseInsensitive) == 0 ? "PNG" : "JPEG", *cancel);
        if (format == ExportFormat::ScanPdf)
            return processing::PdfExporter::write(snapshot, path, false, *cancel, progress,
                                                  processing::PdfExporter::Mode::ImageOnly);
        if (format == ExportFormat::ScientificBook)
            return processing::ScientificBookExporter::write(snapshot, path, *cancel, progress);
        if (format == ExportFormat::LatexDocument)
            return processing::LatexDocumentExporter::write(snapshot,path,*cancel,progress);
        if (format == ExportFormat::Samples)
            return processing::VerifiedSampleExporter::write(snapshot, path, *cancel, progress);
        return processing::PdfExporter::write(snapshot, path, format == ExportFormat::ArchivalPdf, *cancel, progress,
            format == ExportFormat::FormattedPdf ? processing::PdfExporter::Mode::FormattedText :
                                                  processing::PdfExporter::Mode::SearchableScan,
            typesetFormulas ? processing::PdfExporter::FormulaStyle::Typeset : processing::PdfExporter::FormulaStyle::Scanned);
    }));
}

} // namespace yagf::ui

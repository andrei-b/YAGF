#include "mainwindow.h"
#include "projectfile.h"

#include <QCloseEvent>
#include <QActionGroup>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGraphicsPixmapItem>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QHBoxLayout>
#include <QImageReader>
#include <QIcon>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMouseEvent>
#include <QMenuBar>
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
#include <QSplitter>
#include <QSpinBox>
#include <QStatusBar>
#include <QToolBar>
#include <QVBoxLayout>

#include <utility>

namespace yagf::ui {
using core::OcrDocument;
using core::OcrPage;
using core::OcrWord;
using core::ProjectFile;
using processing::DocumentProcessor;

namespace {
bool overlapsWord(const QRect &box, const QRect &selection)
{
    if (selection.contains(box.center())) return true;
    const QRect overlap = box.intersected(selection);
    return !overlap.isEmpty() &&
        static_cast<qint64>(overlap.width()) * overlap.height() * 4 >=
        static_cast<qint64>(box.width()) * box.height();
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

MainWindow::MainWindow()
{
    updateTitle();
    resize(1200, 800);
    auto *fileMenu = menuBar()->addMenu(tr("&File"));
    auto *toolbar = addToolBar(tr("Document"));
    fileMenu->addAction(toolbar->addAction(tr("Open image or PDF"), this, &MainWindow::openFile));
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
    ocrButton = new QPushButton(tr("Recognize"), this);
    toolbar->addWidget(ocrButton);
    connect(ocrButton, &QPushButton::clicked, this, &MainWindow::runOcr);
    cancelButton = new QPushButton(tr("Cancel"), this);
    cancelButton->setEnabled(false);
    toolbar->addWidget(cancelButton);
    connect(cancelButton, &QPushButton::clicked, &processor, &DocumentProcessor::cancel);
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
    updatePageControls();
    connect(pageList, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row >= 0) showPage(row + 1);
    });
    connect(view, &PageView::regionSelected, this, &MainWindow::selectRegion);
    connect(applyButton, &QPushButton::clicked, this, &MainWindow::applyCorrection);
    connect(regionOcrButton, &QPushButton::clicked, this, &MainWindow::runRegionOcr);
    connect(selectionEditor, &QTextEdit::textChanged, this, [this] { editorPending = true; });
    connect(view, &PageView::wordClicked, this, &MainWindow::selectWord);
    connect(wordList, &QListWidget::currentRowChanged, this, &MainWindow::selectWord);
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
        const QPixmap miniature(path);
        if (!miniature.isNull())
            pageList->item(number - 1)->setIcon(QIcon(miniature.scaled(
                pageList->iconSize(), Qt::KeepAspectRatio, Qt::SmoothTransformation)));
    });
    connect(&processor, &DocumentProcessor::pageRendered, this, [this](int number, const QString &path) {
        if (number == currentPage) loadImage(path);
    });
    connect(&processor, &DocumentProcessor::recognized, this,
            [this](const QByteArray &tsv, const QPoint &offset, const QRect &region) {
        QImageReader reader(imagePath);
        const QSize size = reader.size();
        const QSize recognizedSize = region.isEmpty() ? size : region.size();
        OcrDocument result = OcrDocument::fromTsv(tsv, recognizedSize.width(), recognizedSize.height(),
                                                  originalPath, languageEdit->text().trimmed(), currentPage);
        OcrPage *newPage = result.page(currentPage);
        const OcrPage *oldPage = document.page(currentPage);
        for (OcrWord &word : newPage->words) {
            word.bounds.translate(offset);
            if (!region.isEmpty()) word.id.clear();
            if (!oldPage) continue;
            for (const OcrWord &old : oldPage->words) {
                if (old.bounds == word.bounds && old.originalText == word.originalText) {
                    word.id = old.id;
                    word.bold = old.bold;
                    word.italic = old.italic;
                    word.fontSizePt = old.fontSizePt;
                    word.sourceFontSizePt = old.sourceFontSizePt;
                    if (old.reviewed) {
                        word.text = old.text;
                        word.reviewed = true;
                    }
                    break;
                }
            }
        }
        document.sourcePath = originalPath;
        document.engine = result.engine;
        document.language = result.language;
        document.totalPages = pageCount;
        if (region.isEmpty()) {
            document.replacePage(std::move(*newPage));
        } else {
            OcrPage merged;
            if (oldPage) merged = *oldPage;
            merged.id = QStringLiteral("page-%1").arg(currentPage);
            merged.number = currentPage;
            merged.width = size.width();
            merged.height = size.height();
            merged.engineId = result.engine;
            merged.language = result.language;
            int maxBlock = 0;
            for (const OcrWord &old : merged.words) maxBlock = qMax(maxBlock, old.block);
            for (OcrWord &word : newPage->words) {
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
            for (const OcrWord &word : newPage->words) kept.insert(insertion++, word);
            merged.words = std::move(kept);
            document.replacePage(std::move(merged));
        }
        showDocument();
        if (!region.isEmpty()) selectRegion(region);
        markDirty();
        status->setText(tr("Page %1: %2 words recognized").arg(currentPage).arg(document.page(currentPage)->words.size()));
    });
}

void MainWindow::setBusy(bool busy)
{
    ocrButton->setEnabled(!busy && !imagePath.isEmpty());
    cancelButton->setEnabled(busy);
    regionOcrButton->setEnabled(!busy && !imagePath.isEmpty() && !selectedRegion.isEmpty());
    updatePageControls();
}

void MainWindow::updatePageControls()
{
    const QSignalBlocker blocker(pageSpin);
    pageSpin->setMaximum(pageCount);
    pageSpin->setValue(currentPage);
    const bool available = !processor.isBusy();
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
}

void MainWindow::showError(const QString &message)
{
    status->setText(message);
    QMessageBox::warning(this, tr("YAGF Revival"), message);
}

void MainWindow::openFile()
{
    const QString path = QFileDialog::getOpenFileName(this, tr("Open document"), {},
                                                     tr("Documents (*.pdf *.png *.jpg *.jpeg *.tif *.tiff *.bmp);;All files (*)"));
    loadSource(path);
}

void MainWindow::loadSource(const QString &path)
{
    if (path.isEmpty()) return;
    if (processor.isBusy()) return;
    if (!maybeSave()) return;
    processor.resetSource();
    originalPath = QFileInfo(path).absoluteFilePath();
    projectPath.clear();
    document = {};
    document.sourcePath = originalPath;
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
    ocrButton->setEnabled(false);
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
    if (processor.isBusy()) return;
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
    originalPath = loaded.sourcePath;
    projectPath = path;
    document = std::move(loaded);
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
    dirty = true;
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
    const QImage image = reader.read();
    if (image.isNull()) {
        showError(tr("Could not read image: %1").arg(reader.errorString()));
        return;
    }
    imagePath = path;
    const OcrPage *page = document.page(currentPage);
    if (page && (page->width != image.width() || page->height != image.height())) {
        imagePath.clear();
        showError(tr("The source image size changed since this project was saved."));
        return;
    }
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
    status->setText(tr("Image loaded: %1 × %2").arg(image.width()).arg(image.height()));
}

void MainWindow::showPage(int number)
{
    if (number < 1 || number > pageCount || processor.isBusy()) return;
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
    ocrButton->setEnabled(false);
    if (originalPath.endsWith(QStringLiteral(".pdf"), Qt::CaseInsensitive)) {
        status->setText(tr("Rendering page %1 of %2…").arg(number).arg(pageCount));
        processor.renderPdfPage(number);
    } else {
        loadImage(originalPath);
    }
}

void MainWindow::runOcr()
{
    if (imagePath.isEmpty() || processor.isBusy()) return;
    if (!maybeApplyEditor()) return;
    status->setText(tr("Recognizing…"));
    processor.recognize(imagePath, languageEdit->text().trimmed());
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
    if (!page) { updateSelection({}); return; }
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
}

void MainWindow::selectWord(int index)
{
    if (index < 0 || index >= wordBoxes.size()) return;
    if (!maybeApplyEditor()) return;
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
    applyButton->setEnabled(!indices.isEmpty());
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

bool MainWindow::applyCorrection()
{
    if (selectedWords.isEmpty()) { editorPending = false; return true; }
    const QString plain = selectionEditor->toPlainText();
    QRegularExpression wordPattern(QStringLiteral("\\S+"));
    auto matches = wordPattern.globalMatch(plain);
    QVector<QRegularExpressionMatch> tokens;
    while (matches.hasNext()) tokens.append(matches.next());
    if (tokens.size() != selectedWords.size()) {
        QMessageBox::information(this, tr("Keep source boxes aligned"),
            tr("This selection has %1 source words. Edit their text and formatting while keeping %1 words, then apply.")
                .arg(selectedWords.size()));
        return false;
    }
    const QSignalBlocker listBlocker(wordList);
    for (int i = 0; i < tokens.size(); ++i) {
        const int index = selectedWords.at(i);
        OcrWord *word = displayedWords.at(index);
        word->text = tokens.at(i).captured();
        word->reviewed = true;
        QTextCursor cursor(selectionEditor->document());
        cursor.setPosition(tokens.at(i).capturedStart() + 1);
        const QTextCharFormat format = cursor.charFormat();
        word->bold = format.fontWeight() >= QFont::Bold;
        word->italic = format.fontItalic();
        if (format.fontPointSize() > 0) word->fontSizePt = qRound(format.fontPointSize());
        wordList->item(index)->setText(word->text);
        wordList->item(index)->setForeground(QColor(0, 90, 20));
    }
    markDirty();
    editorPending = false;
    statusBar()->showMessage(tr("Text and formatting applied to source-linked words."));
    return true;
}

bool MainWindow::maybeApplyEditor()
{
    if (!editorPending) return true;
    const QRegularExpression wordPattern(QStringLiteral("\\S+"));
    int count = 0;
    auto matches = wordPattern.globalMatch(selectionEditor->toPlainText());
    while (matches.hasNext()) { matches.next(); ++count; }
    if (count == selectedWords.size()) return applyCorrection();
    const auto choice = QMessageBox::warning(this, tr("Unapplied text"),
        tr("The edited text has %1 words, but the selection has %2 source boxes. Discard these unapplied edits?")
            .arg(count).arg(selectedWords.size()), QMessageBox::Discard | QMessageBox::Cancel,
        QMessageBox::Cancel);
    if (choice != QMessageBox::Discard) return false;
    updateSelection(selectedWords);
    return true;
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
    processor.recognizeRegion(imagePath, languageEdit->text().trimmed(), selectedRegion);
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

} // namespace yagf::ui

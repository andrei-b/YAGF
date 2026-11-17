#include "mainwindow.h"
#include "projectfile.h"

#include <QCloseEvent>
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
#include <QPushButton>
#include <QSignalBlocker>
#include <QSplitter>
#include <QSpinBox>
#include <QStatusBar>
#include <QToolBar>
#include <QTabWidget>
#include <QVBoxLayout>

#include <utility>

PageView::PageView(QWidget *parent) : QGraphicsView(parent) {}

void PageView::mousePressEvent(QMouseEvent *event)
{
    if (auto *box = qgraphicsitem_cast<QGraphicsRectItem *>(itemAt(event->pos()))) {
        const QVariant index = box->data(0);
        if (index.isValid())
            emit wordClicked(index.toInt());
    }
    QGraphicsView::mousePressEvent(event);
}

MainWindow::MainWindow()
{
    updateTitle();
    resize(1200, 800);
    auto *toolbar = addToolBar(tr("Document"));
    toolbar->addAction(tr("Open image or PDF"), this, &MainWindow::openFile);
    toolbar->addAction(tr("Open project"), this, &MainWindow::openProject);
    toolbar->addAction(tr("Save project"), this, &MainWindow::saveProject);
    toolbar->addAction(tr("Save project as…"), this, &MainWindow::saveProjectAs);
    toolbar->addSeparator();
    previousButton = new QPushButton(tr("◀"), this);
    nextButton = new QPushButton(tr("▶"), this);
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
    toolbar->addAction(tr("Export text"), this, &MainWindow::exportText);
    toolbar->addAction(tr("Export JSON"), this, &MainWindow::exportJson);

    auto *splitter = new QSplitter(this);
    view = new PageView(splitter);
    scene = new QGraphicsScene(this);
    view->setScene(scene);
    view->setDragMode(QGraphicsView::ScrollHandDrag);
    view->setRenderHint(QPainter::Antialiasing);
    sidebarTabs = new QTabWidget(splitter);
    pageList = new QListWidget(sidebarTabs);
    pageList->setIconSize(QSize(80, 110));
    wordList = new QListWidget(sidebarTabs);
    wordList->setToolTip(tr("Double-click a word to correct it. Click a word to locate it on the scan."));
    sidebarTabs->addTab(pageList, tr("Pages"));
    sidebarTabs->addTab(wordList, tr("Words"));
    splitter->addWidget(view);
    splitter->addWidget(sidebarTabs);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({900, 300});
    splitter->setChildrenCollapsible(false);
    setCentralWidget(splitter);
    updatePageControls();
    connect(pageList, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row >= 0) showPage(row + 1);
    });
    connect(view, &PageView::wordClicked, this, &MainWindow::selectWord);
    connect(wordList, &QListWidget::currentRowChanged, this, &MainWindow::selectWord);
    connect(wordList, &QListWidget::itemChanged, this, [this](QListWidgetItem *item) {
        const int index = wordList->row(item);
        if (index < 0 || index >= displayedWords.size()) return;
        displayedWords[index]->text = item->text();
        displayedWords[index]->reviewed = true;
        item->setForeground(QColor(0, 90, 20));
        markDirty();
        statusBar()->showMessage(tr("Correction changed; save the project to keep it."));
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
        showPage(1);
    });
    connect(&processor, &DocumentProcessor::pageRendered, this, [this](int number, const QString &path) {
        if (number == currentPage) loadImage(path);
    });
    connect(&processor, &DocumentProcessor::recognized, this, [this](const QByteArray &tsv) {
        QImageReader reader(imagePath);
        const QSize size = reader.size();
        OcrDocument result = OcrDocument::fromTsv(tsv, size.width(), size.height(),
                                                  originalPath, languageEdit->text().trimmed(), currentPage);
        OcrPage *newPage = result.page(currentPage);
        const OcrPage *oldPage = document.page(currentPage);
        // Keep deliberate edits when OCR is rerun on the same page and geometry.
        for (OcrWord &word : newPage->words) {
            if (!oldPage) break;
            for (const OcrWord &old : oldPage->words) {
                if (old.bounds == word.bounds && old.originalText == word.originalText) {
                    word.id = old.id;
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
        document.replacePage(std::move(*newPage));
        showDocument();
        markDirty();
        status->setText(tr("Page %1: %2 words recognized").arg(currentPage).arg(document.page(currentPage)->words.size()));
    });
}

void MainWindow::setBusy(bool busy)
{
    ocrButton->setEnabled(!busy && !imagePath.isEmpty());
    cancelButton->setEnabled(busy);
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
    originalPath = QFileInfo(path).absoluteFilePath();
    projectPath.clear();
    document = {};
    document.sourcePath = originalPath;
    dirty = false;
    updateTitle();
    wordList->clear();
    displayedWords.clear();
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
    originalPath = loaded.sourcePath;
    projectPath = path;
    document = std::move(loaded);
    languageEdit->setText(document.language);
    dirty = false;
    updateTitle();
    wordList->clear();
    displayedWords.clear();
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
    scene->clear();
    scene->addPixmap(QPixmap::fromImage(image));
    if (currentPage >= 1 && currentPage <= pageList->count())
        pageList->item(currentPage - 1)->setIcon(QIcon(QPixmap::fromImage(
            image.scaled(QSize(80, 110), Qt::KeepAspectRatio, Qt::SmoothTransformation))));
    scene->setSceneRect(QRectF(QPointF(0, 0), image.size()));
    view->fitInView(scene->sceneRect(), Qt::KeepAspectRatio);
    showDocument();
    setBusy(false);
    status->setText(tr("Image loaded: %1 × %2").arg(image.width()).arg(image.height()));
}

void MainWindow::showPage(int number)
{
    if (number < 1 || number > pageCount || processor.isBusy()) return;
    currentPage = number;
    updatePageControls();
    if (const OcrPage *page = document.page(number))
        languageEdit->setText(page->language);
    wordList->clear();
    displayedWords.clear();
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
    if (!page) return;
    for (OcrWord &word : page->words) {
        const int i = displayedWords.size();
        displayedWords.append(&word);
        auto *box = scene->addRect(word.bounds, QPen(QColor(20, 90, 220), 1.5));
        box->setBrush(QColor(20, 90, 220, 35));
        box->setData(0, i);
        box->setZValue(1);
        wordBoxes.append(box);
        auto *item = new QListWidgetItem(word.text, wordList);
        item->setFlags(item->flags() | Qt::ItemIsEditable);
        if (word.reviewed) item->setForeground(QColor(0, 90, 20));
        item->setToolTip(tr("Confidence: %1; box: %2, %3, %4 × %5")
                         .arg(word.confidence).arg(word.bounds.x()).arg(word.bounds.y())
                         .arg(word.bounds.width()).arg(word.bounds.height()));
    }
    if (!page->words.isEmpty()) {
        sidebarTabs->setCurrentWidget(wordList);
    }
}

void MainWindow::selectWord(int index)
{
    if (index < 0 || index >= wordBoxes.size()) return;
    for (int i = 0; i < wordBoxes.size(); ++i) {
        wordBoxes[i]->setPen(QPen(i == index ? QColor(220, 50, 20) : QColor(20, 90, 220),
                                  i == index ? 3 : 1.5));
    }
    if (wordList->currentRow() != index) wordList->setCurrentRow(index);
    view->centerOn(wordBoxes.at(index));
}

void MainWindow::exportText()
{
    if (document.wordCount() == 0) return;
    const QString path = QFileDialog::getSaveFileName(this, tr("Export text"), {}, tr("Text (*.txt)"));
    if (path.isEmpty()) return;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) { showError(file.errorString()); return; }
    file.write(document.plainText().toUtf8());
}

void MainWindow::exportJson()
{
    if (document.wordCount() == 0) return;
    const QString path = QFileDialog::getSaveFileName(this, tr("Export structured OCR"), {}, tr("JSON (*.json)"));
    if (path.isEmpty()) return;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) { showError(file.errorString()); return; }
    file.write(QJsonDocument(document.toJson()).toJson(QJsonDocument::Indented));
}

#include "pdfexporter.h"
#include "detail/pdfformulas.h"
#include "formulaeditor.h"
#include "exportpage.h"
#include "scientificbookexporter.h"
#include "latexdocumentexporter.h"
#include "docxexporter.h"
#include "verifiedsampleexporter.h"
#include "mainwindow.h"
#include "regionoutline.h"
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

// Export commands of MainWindow: format selection, destination choice and the
// background export job. Split from mainwindow.cpp.
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
class ExportSaveDialog : public QFileDialog {
public:
    using QFileDialog::QFileDialog;
    bool imageExport = false;
    QHash<QString, QStringList> pageExtensions;
    void accept() override
    {
        if (!pageExtensions.isEmpty()) {
            const auto extensions = pageExtensions.value(selectedNameFilter());
            setDefaultSuffix(extensions.first());
            const QFileInfo file(selectedFiles().value(0));
            if (!file.isDir() && !extensions.contains(file.suffix().toLower())) {
                QMessageBox::information(this, windowTitle(), tr("Choose a filename ending in .%1 for this format.").arg(extensions.first()));
                return;
            }
        }
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
} // namespace

void MainWindow::exportText()
{
    if (!maybeApplyEditor()) return;
    if (document.wordCount() == 0) return;
    chooseExportPath(tr("Export text"), tr("Text (*.txt)"), "txt", ExportFormat::PlainText);
}

void MainWindow::exportHtml()
{
    if (!maybeApplyEditor() || document.wordCount() == 0) return;
    chooseExportPath(tr("Export layout HTML"), tr("HTML (*.html)"), "html", ExportFormat::LayoutHtml);
}

void MainWindow::exportJson()
{
    if (!maybeApplyEditor() || document.wordCount() == 0) return;
    chooseExportPath(tr("Export structured OCR"), tr("JSON (*.json)"), "json", ExportFormat::Json);
}

void MainWindow::exportDocument(ExportFormat format)
{
    if (processor.isBusy() || exportBusy || projectLoading || document.pages.isEmpty() || !maybeApplyEditor()) return;
    const auto count = format == ExportFormat::Docx
        ? std::ranges::count_if(document.pages, processing::hasExportContent) : document.pages.size();
    if (count < pageCount && QMessageBox::question(this, tr("Unrecognized pages"),
        tr("Export only the recognized pages?"), QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) return;
    if (format == ExportFormat::Docx)
        chooseExportPath(tr("Export Word document"), tr("Word document (*.docx)"), "docx", format);
    else
        chooseExportPath(tr("Export LaTeX document"), tr("LaTeX document (*.tex)"), "tex", format);
}

void MainWindow::exportCurrentPage()
{
    if (processor.isBusy() || exportBusy || projectLoading || imagePath.isEmpty() || !maybeApplyEditor()) return;
    const int number = currentPage;
    ExportSaveDialog dialog(this, tr("Export current page"));
    dialog.setOption(QFileDialog::DontUseNativeDialog);
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    QStringList filters;
    QHash<QString, ExportFormat> formats;
    const auto add = [&](ExportFormat format, const QString &filter, QStringList extensions) {
        filters.append(filter);
        formats.insert(filter, format);
        dialog.pageExtensions.insert(filter, std::move(extensions));
    };
    const auto *page = document.page(number);
    if (page && processing::hasExportContent(*page)) {
        add(ExportFormat::Docx, tr("Word document (*.docx)"), {"docx"});
        add(ExportFormat::PlainText, tr("Plain text (*.txt)"), {"txt"});
        add(ExportFormat::LayoutHtml, tr("Layout HTML (*.html)"), {"html"});
        add(ExportFormat::ScientificBook, tr("Scientific book HTML (*.html)"), {"html"});
        add(ExportFormat::LatexDocument, tr("LaTeX document (*.tex)"), {"tex"});
        add(ExportFormat::Json, tr("Structured OCR (*.json)"), {"json"});
    }
    add(ExportFormat::Pdf, tr("Page as PDF (*.pdf)"), {"pdf"});
    add(ExportFormat::ScanImage, tr("PNG image (*.png)"), {"png"});
    add(ExportFormat::ScanImage, tr("JPEG image (*.jpg *.jpeg)"), {"jpg", "jpeg"});
    dialog.setNameFilters(filters);
    dialog.setDefaultSuffix(dialog.pageExtensions.value(filters.first()).first());
    connect(&dialog, &QFileDialog::filterSelected, &dialog, [&](const QString &filter) {
        dialog.setDefaultSuffix(dialog.pageExtensions.value(filter).first());
    });
    dialog.selectFile(tr("%1-page-%2.%3").arg(QFileInfo(originalPath).completeBaseName()).arg(number).arg(dialog.defaultSuffix()));
    if (dialog.exec() == QDialog::Accepted)
        startExport(dialog.selectedFiles().value(0), formats.value(dialog.selectedNameFilter()), false, number);
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
    const bool typesetFormulas = typeset && typeset->isChecked();
    if (typeset && typeset->isEnabled()) settings.setValue(typesetKey, typesetFormulas);
    startExport(path, format, typesetFormulas);
}

void MainWindow::startExport(const QString &path, ExportFormat format, bool typesetFormulas, std::optional<int> onlyPage)
{
    if (path.isEmpty() || exportBusy || processor.isBusy() || projectLoading) return;
    if (QFileInfo(path) == QFileInfo(originalPath) || (!projectPath.isEmpty() && QFileInfo(path) == QFileInfo(projectPath))) {
        showError(tr("Choose a destination separate from the source and project files."));
        return;
    }
    exportPath = path;
    exportCancel = std::make_shared<std::atomic_bool>(false);
    exportBusy = true;
    setBusy(true);
    status->setText(tr("Preparing export…"));
    OcrDocument snapshot = document;
    if (onlyPage) {
        // Keep the original page number for PDF renders, figure crops, and source-linked OCR.
        const auto *page = document.page(*onlyPage);
        snapshot.pages = page ? QVector<OcrPage>{*page} : QVector<OcrPage>{};
        // Book-level training rules can contain image crops from other pages.
        snapshot.symbolRules.clear();
    } else if (format == ExportFormat::Docx) {
        snapshot.pages.removeIf([](const auto &page) { return !processing::hasExportContent(page); });
    }
    const auto cancel = exportCancel;
    exportWatcher.setFuture(QtConcurrent::run([this, snapshot, path, format, typesetFormulas, cancel, onlyPage, pageNumber = onlyPage.value_or(currentPage)] {
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
        if (format == ExportFormat::Docx)
            return processing::DocxExporter::write(snapshot, path, *cancel, progress);
        if (format == ExportFormat::PlainText || format == ExportFormat::LayoutHtml || format == ExportFormat::Json) {
            QByteArray data;
            if (format == ExportFormat::PlainText) data = snapshot.plainText().toUtf8();
            else if (format == ExportFormat::LayoutHtml) data = snapshot.layoutHtml().toUtf8();
            else data = QJsonDocument(snapshot.toJson(false)).toJson(QJsonDocument::Indented);
            QSaveFile output(path);
            if (cancel->load()) return processing::ExportResult{.status = processing::ExportResult::Status::Cancelled};
            if (!output.open(QIODevice::WriteOnly) || output.write(data) != data.size())
                return processing::ExportResult{.error = output.errorString()};
            if (cancel->load()) return processing::ExportResult{.status = processing::ExportResult::Status::Cancelled};
            if (!output.commit()) return processing::ExportResult{.error = output.errorString()};
            return processing::ExportResult{.status = processing::ExportResult::Status::Success};
        }
        if (format == ExportFormat::Samples)
            return processing::VerifiedSampleExporter::write(snapshot, path, *cancel, progress);
        return processing::PdfExporter::write(snapshot, path, format == ExportFormat::ArchivalPdf, *cancel, progress,
            format == ExportFormat::FormattedPdf ? processing::PdfExporter::Mode::FormattedText :
                                                  processing::PdfExporter::Mode::SearchableScan,
            typesetFormulas ? processing::PdfExporter::FormulaStyle::Typeset : processing::PdfExporter::FormulaStyle::Scanned, onlyPage);
    }));
}

} // namespace yagf::ui

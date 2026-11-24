#include "mainwindow.h"
#include "exportpage.h"
#include "latexdocumentexporter.h"
#include "docxexporter.h"
#include "projectfile.h"
#include "verificationcontroller.h"
#include "correctionretention.h"
#include "imageprocessor.h"
#include "symbolfixture.h"
#include "symbolrulesdialog.h"
#include "formulatext.h"
#include "aipagelayoutdialog.h"
#include <QTcpServer>
#include <QTcpSocket>
#include <QJsonDocument>
#include <QClipboard>
#include <QListView>
#include <QPdfWriter>
#include <QPainter>
#include <QPageSize>
#include <QSpinBox>

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QContextMenuEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QImageReader>
#include <QFileInfo>
#include <QGraphicsRectItem>
#include <QGraphicsPixmapItem>
#include <QGraphicsScene>
#include <QImage>
#include <QLineEdit>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QProcess>
#include <QGraphicsView>
#include <QProgressBar>
#include <QProgressDialog>
#include <QRubberBand>
#include <QSignalSpy>
#include <QScrollBar>
#include <QPlainTextEdit>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QThread>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QWheelEvent>
#include <QUrl>
#include <QTest>
#include <QSplitter>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <functional>
namespace processing = yagf::processing;

static void testInlineTarget(const QTemporaryDir &directory, const QString &sourcePath, bool table = false)
{
    using namespace yagf;
    const QString original = table ? QStringLiteral("Столбец\tЗначение\nСтрока\tс=2") :
        QStringLiteral(R"(Исправлено: \(a=1\), затем с=2, где с — число.)");
    const QString expected = QString(original).replace("с=2", R"(\(c=2\))");
    core::OcrDocument book;
    book.sourcePath = sourcePath;
    core::OcrPage page;
    page.width = page.sourceWidth = 200;
    page.height = page.sourceHeight = 100;
    page.words = {{1,1,1,1,{0,0,200,100},"raw OCR",original,90,true}};
    page.words.first().layoutBlock = true;
    page.rebuildHierarchy();
    QJsonObject header{{"header",true},{"rowspan",1},{"colspan",1}};
    QJsonObject cell = header; cell["header"] = false;
    QJsonArray layout{QJsonArray{header,header},QJsonArray{header,cell}};
    if (table) { page.regions.first().type="table"; page.regions.first().tableLayout=layout; }
    book.replacePage(page);
    QString error;
    const auto path = directory.filePath("inline-test.yagf.json");
    assert(core::ProjectFile::save(path, book, &error));
    const auto bin = directory.filePath("yagf2/math-ocr/venv/bin");
    assert(QDir().mkpath(bin));
    QFile engine(bin + "/python");
    assert(engine.open(QIODevice::WriteOnly));
    engine.write("#!/bin/sh\nif [ -e \"$0.fail\" ]; then exit 2; fi\nprintf '%s' '[{\"box\":[0,0,10,10],\"text\":\"c=2\"}]'\n");
    engine.close();
    assert(engine.setPermissions(QFileDevice::ReadOwner | QFileDevice::ExeOwner));
    ui::MainWindow window;
    window.loadProject(path);
    auto *editor = window.findChild<QTextEdit *>("recognizedTextEditor");
    auto *words = window.findChild<QListWidget *>("recognizedWords");
    words->setCurrentRow(0, QItemSelectionModel::ClearAndSelect);
    QFile failure(engine.fileName()+".fail");
    for (int attempt = 0; attempt < 3; ++attempt) {
        const bool accept = attempt == 2;
        if (attempt == 1) { assert(failure.open(QIODevice::WriteOnly)); failure.close(); }
        if (accept) assert(failure.remove());
        auto cursor = editor->textCursor();
        cursor.setPosition(original.indexOf("с=2"));
        cursor.setPosition(cursor.position()+3, QTextCursor::KeepAnchor);
        editor->setTextCursor(cursor);
        QTimer::singleShot(0, &window, [&] {
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget()); assert(menu);
            QTimer::singleShot(0, &window, [&] {
                auto *dialog = QApplication::activeModalWidget();
                assert(dialog && dialog->objectName()=="inlineFormulaTarget");
                assert(dialog->findChild<QPlainTextEdit *>()->textCursor().selectedText()=="с=2");
                if (attempt == 0) { qobject_cast<QDialog *>(dialog)->reject(); return; }
                auto *view = dialog->findChild<QGraphicsView *>();
                view->rubberBandChanged(QRect(2,2,30,20), QPointF(4,5), QPointF(34,25));
                QTimer *poll = new QTimer(dialog);
                QObject::connect(poll, &QTimer::timeout, dialog, [poll,dialog,accept] {
                    if (!accept) {
                        for (auto *button : dialog->findChildren<QPushButton *>())
                            if (button->text()=="Recognize crop" && button->isEnabled()) {
                                poll->stop(); qobject_cast<QDialog *>(dialog)->reject(); return;
                            }
                    }
                    auto *preview=qobject_cast<QDialog *>(QApplication::activeModalWidget());
                    if (preview && preview->objectName()=="formulaEditor") {
                        assert(preview->findChild<QPlainTextEdit *>("formulaSource")->toPlainText()=="c=2");
                        poll->stop(); preview->accept();
                    }
                });
                poll->start(10);
                for (auto *button : dialog->findChildren<QPushButton *>())
                    if (button->text()=="Recognize crop") button->click();
            });
            for (auto *action : menu->actions()) if (action->text()=="Mark as inline formula…") action->trigger();
            menu->close();
        });
        editor->customContextMenuRequested({});
        assert(words->item(0)->text()==(accept ? expected : original));
    }
    const auto trigger = [&](QString name) {
        for (auto *action: window.findChildren<QAction *>()) if (action->text()==name) { action->trigger(); return; }
        assert(false);
    };
    trigger("Undo"); assert(words->item(0)->text()==original);
    trigger("Redo"); assert(words->item(0)->text()==expected);
    trigger("Save project");
    assert(core::ProjectFile::load(path,&book,&error));
    assert(book.page(1)->words.first().text==expected && book.page(1)->words.first().originalText=="raw OCR");
    if (table) assert(book.page(1)->regions.first().tableLayout == layout);
    assert(book.page(1)->recognitionHistory.decisions.size()==1);
    assert(book.page(1)->recognitionHistory.decisions.first().processedArea==QRect(4,5,30,20));
    window.loadProject(path);
    assert(words->item(0)->text()==expected);
    const auto tex = directory.filePath("inline-test.tex");
    assert(processing::LatexDocumentExporter::write(book,tex,false).status==processing::ExportResult::Status::Success);
    QFile output(tex); assert(output.open(QIODevice::ReadOnly));
    const auto latex = QString::fromUtf8(output.readAll());
    if (table) assert(latex.contains("longtable") && latex.contains(R"(\textbf{Строка} & \(c=2\))"));
    else assert(latex.contains(expected));
    if (!QStandardPaths::findExecutable("pandoc").isEmpty() && !QStandardPaths::findExecutable("unzip").isEmpty()) {
        const auto docx = directory.filePath("inline-test.docx");
        assert(processing::DocxExporter::write(book,docx,false).status==processing::ExportResult::Status::Success);
        QProcess unzip; unzip.start("unzip", {"-p",docx,"word/document.xml"});
        assert(unzip.waitForFinished());
        const auto xml = unzip.readAllStandardOutput();
        assert(xml.count("<m:oMath>")==(table?1:2) && !xml.contains("<m:oMathPara>"));
        if (table) assert(xml.contains("<w:tbl>") && xml.contains("Строка") && xml.contains("Значение"));
        else assert(xml.contains("Исправлено") && xml.contains("затем") && xml.contains("число."));
    }
    assert(engine.remove());
}

static void testUnrecognizedProjectSaving(const QTemporaryDir &directory)
{
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    const auto wait = [](const std::function<bool()> &ready) {
        QElapsedTimer timer; timer.start();
        while (!ready() && timer.elapsed() < 10000) { QApplication::processEvents(); QThread::msleep(5); }
        assert(ready());
    };
    const auto imagePath = directory.filePath("unrecognized.png");
    QImage image(200, 100, QImage::Format_RGB32); image.fill(Qt::red); assert(image.save(imagePath));
    const auto pdfPath = directory.filePath("unrecognized.pdf");
    {
        QPdfWriter writer(pdfPath);
        writer.setResolution(150);
        writer.setPageSize(QPageSize(QSizeF(100, 120), QPageSize::Millimeter));
        writer.setPageMargins(QMarginsF(0, 0, 0, 0));
        QPainter painter(&writer);
        painter.fillRect(0, 0, writer.width(), writer.height(), Qt::red);
        assert(writer.newPage());
        painter.fillRect(0, 0, writer.width(), writer.height(), Qt::blue);
    }
    for (const auto &source : {imagePath, pdfPath}) {
        yagf::ui::MainWindow window;
        window.show();
        window.loadSource(source);
        auto *pages = window.findChild<QListWidget *>("pageMiniatures");
        auto *recognize = window.findChild<QAction *>("recognizeAllAction");
        auto *view = window.findChild<yagf::ui::PageView *>();
        const int count = source == pdfPath ? 2 : 1;
        wait([&] { return recognize->isEnabled() && pages->count() == count; });
        QAction *save = nullptr;
        for (auto *action : window.findChildren<QAction *>())
            if (action->text() == "Save project") save = action;
        assert(save);
        const QStringList actions{"Save project", "Save project as…", "Save portable project folder…"};
        for (int i = 0; i < actions.size(); ++i) {
            QAction *saveAction = nullptr;
            for (auto *action : window.findChildren<QAction *>())
                if (action->text() == actions[i]) saveAction = action;
            assert(saveAction);
            const bool portable = i == 2;
            const auto destination = directory.filePath(QFileInfo(source).fileName() + QString::number(i) +
                (portable ? ".yagfproj" : ".yagf.json"));
            QTimer::singleShot(0, &window, [&] {
                auto *dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget()); assert(dialog);
                dialog->selectFile(destination);
                QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection);
            });
            saveAction->trigger();
            yagf::core::OcrDocument saved;
            QString error;
            const auto load = [&] {
                return portable ? yagf::core::ProjectFile::loadPortable(destination, &saved, &error)
                                : yagf::core::ProjectFile::load(destination, &saved, &error);
            };
            assert(load() && saved.totalPages == count && saved.pages.isEmpty() && saved.engine.isEmpty());
            window.loadProject(destination);
            wait([&] { return recognize->isEnabled() && pages->count() == count; });
            // A saved source can be reopened and resaved without creating OCR records.
            window.activateWindow();
            wait([&] { return QApplication::activeWindow() == &window && !QApplication::activeModalWidget(); });
            view->setFocus();
            QSignalSpy saveInvoked(save, &QAction::triggered);
            QTest::keyClick(window.windowHandle(), Qt::Key_S, Qt::ControlModifier);
            assert(saveInvoked.size() == 1);
            assert(load() && saved.pages.isEmpty());
            for (int number = 1; number <= count; ++number) {
                pages->setCurrentRow(number - 1);
                wait([&] { return recognize->isEnabled(); });
                bool foundImage = false;
                for (auto *item : view->scene()->items())
                    if (auto *pixmap = qgraphicsitem_cast<QGraphicsPixmapItem *>(item)) {
                        const auto displayed = pixmap->pixmap().toImage();
                        assert(displayed.pixelColor(displayed.width()/2, displayed.height()/2) ==
                            (number == 1 ? QColor(Qt::red) : QColor(Qt::blue)));
                        foundImage = true;
                    }
                assert(foundImage);
            }
        }
    }
}

static void testScannedPages(const QTemporaryDir &directory)
{
    using yagf::core::OcrDocument;
    using yagf::core::ProjectFile;
    const auto wait = [](const std::function<bool()> &ready) {
        QElapsedTimer timer; timer.start();
        while (!ready() && timer.elapsed() < 10000) { QApplication::processEvents(); QThread::msleep(5); }
        assert(ready());
    };
    QFile scanner(directory.filePath("scanimage"));
    assert(scanner.open(QIODevice::WriteOnly));
    scanner.write("#!/bin/sh\n"
        "case \"$1\" in --formatted-device-list=*) printf 'virtual:0\\tTest scanner\\n'; exit 0;; esac\n"
        "for arg do case \"$arg\" in --output-file=*) cp \"$YAGF_PAGE_FIXTURE\" \"${arg#--output-file=}\";; esac; done\n");
    scanner.close();
    assert(scanner.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    yagf::ui::MainWindow window;
    window.show();
    auto *pages = window.findChild<QListWidget *>("pageMiniatures");
    QAction *scan = nullptr, *save = nullptr, *undo = nullptr, *redo = nullptr;
    for (auto *action : window.findChildren<QAction *>()) {
        if (action->iconText() == "Scan") scan = action;
        if (action->text() == "Save project") save = action;
        if (action->shortcut() == QKeySequence::Undo) undo = action;
        if (action->shortcut() == QKeySequence::Redo) redo = action;
    }
    assert(scan && save && undo && redo);
    QTest::keyClick(pages, Qt::Key_Delete); // No document means no removal prompt.
    const auto scanPage = [&](QColor color, QSize size) {
        QImage fixture(size, QImage::Format_RGB32); fixture.fill(color);
        const auto path = directory.filePath("new-scan.png"); assert(fixture.save(path));
        qputenv("YAGF_PAGE_FIXTURE", QFile::encodeName(path));
        QTimer start;
        QObject::connect(&start, &QTimer::timeout, &window, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog) return;
            for (auto *button : dialog->findChildren<QPushButton *>())
                if (button->text() == "Scan" && button->isEnabled()) { start.stop(); button->click(); return; }
        });
        start.start(5);
        scan->trigger();
        assert(!start.isActive());
        wait([&] { return scan->isEnabled(); });
    };
    scanPage(Qt::red, {80, 100});
    assert(pages->count() == 1);
    undo->trigger(); assert(pages->count() == 0);
    redo->trigger(); assert(pages->count() == 1);
    scanPage(Qt::blue, {90, 110});
    assert(pages->count() == 2 && pages->currentRow() == 1);
    undo->trigger(); assert(pages->count() == 1);
    redo->trigger(); assert(pages->count() == 2 && pages->currentRow() == 1);
    const auto project = directory.filePath("scanned-pages.yagf.json");
    QTimer::singleShot(0, &window, [&] {
        auto *dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget()); assert(dialog);
        dialog->selectFile(project); QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection);
    });
    save->trigger();
    QString error;
    OcrDocument saved; assert(ProjectFile::load(project, &saved, &error));
    assert(saved.totalPages == 2 && saved.pages.size() == 2 && saved.pageSources.size() == 2);
    std::atomic_bool cancelled = false;
    for (int number = 1; number <= 2; ++number) {
        auto image = processing::readExportPage(saved, number, directory, cancelled);
        assert(image && image->pixelColor(0, 0) == (number == 1 ? QColor(Qt::red) : QColor(Qt::blue)));
    }
    // Recognized text and stable IDs survive appending and deleting preceding pages.
    auto *second = saved.page(2);
    second->words.append({2, 1, 1, 1, QRect(10, 10, 20, 10), "original", "corrected", 99, true});
    second->rebuildHierarchy();
    const auto wordId = second->words.first().id;
    assert(ProjectFile::save(project, saved, &error));
    window.loadProject(project);
    wait([&] { return scan->isEnabled(); });
    assert(pages->count() == 2);
    scanPage(Qt::green, {100, 120});
    assert(pages->count() == 3);
    save->trigger();
    assert(ProjectFile::load(project, &saved, &error));
    assert(saved.page(2)->words.first().text == "corrected");
    const auto portable = directory.filePath("scans.yagfproj");
    assert(ProjectFile::savePortable(portable, saved, &error));
    OcrDocument bundled; assert(ProjectFile::loadPortable(portable, &bundled, &error));
    assert(bundled.pageSources.size() == 3);
    auto blue = processing::readExportPage(bundled, 2, directory, cancelled);
    assert(blue && blue->pixelColor(0, 0) == QColor(Qt::blue));
    // Delete applies to the focused thumbnail and defaults to keeping the page.
    pages->setCurrentRow(1);
    pages->setFocus();
    const auto confirm = [&](QMessageBox::StandardButton answer, int number) {
        QTimer::singleShot(0, &window, [&, answer, number] {
            auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()); assert(dialog);
            assert(dialog->windowTitle() == "Remove page" && dialog->text().contains(QString::number(number)));
            assert(dialog->defaultButton() == dialog->button(QMessageBox::No));
            dialog->button(answer)->click();
        });
    };
    confirm(QMessageBox::No, 2);
    QTest::keyClick(pages, Qt::Key_Delete);
    assert(pages->count() == 3);
    // Deleting text does not remove a page or open a confirmation.
    auto *textEditor = window.findChild<QTextEdit *>("recognizedTextEditor");
    textEditor->moveCursor(QTextCursor::End);
    QTest::keyClick(textEditor, Qt::Key_Delete);
    assert(pages->count() == 3);
    // The context menu targets the clicked thumbnail, even if another page is selected.
    pages->setCurrentRow(2);
    pages->scrollToItem(pages->item(0)); pages->doItemsLayout();
    const QPoint position = pages->visualItemRect(pages->item(0)).center();
    QTimer::singleShot(0, &window, [&] {
        auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget()); assert(menu);
        auto *remove = menu->findChild<QAction *>("removePageAction"); assert(remove && remove->isEnabled());
        confirm(QMessageBox::Yes, 1);
        remove->trigger(); menu->close();
    });
    QContextMenuEvent context(QContextMenuEvent::Mouse, position, pages->viewport()->mapToGlobal(position));
    QApplication::sendEvent(pages->viewport(), &context);
    assert(pages->count() == 2 && pages->currentRow() == 1);
    save->trigger();
    assert(ProjectFile::load(project, &saved, &error));
    assert(saved.page(1)->words.first().text == "corrected");
    assert(saved.page(1)->words.first().page == 1 && saved.page(1)->words.first().id == wordId);
    undo->trigger(); assert(pages->count() == 3);
    redo->trigger(); assert(pages->count() == 2);
    pages->setFocus();
    confirm(QMessageBox::Yes, 2);
    QTest::keyClick(pages, Qt::Key_Delete);
    assert(pages->count() == 1 && pages->currentRow() == 0);
    confirm(QMessageBox::Yes, 1);
    QTest::keyClick(pages, Qt::Key_Delete);
    assert(pages->count() == 0 && !window.findChild<QAction *>("recognizeAllAction")->isEnabled());
    save->trigger();
    assert(ProjectFile::load(project, &saved, &error) && saved.totalPages == 0);
    window.loadProject(project);
    assert(pages->count() == 0);
    scanPage(Qt::yellow, {70, 90});
    assert(pages->count() == 1);
    save->trigger();
    window.loadProject(project);
    assert(pages->count() == 1);
    // Physical PDF page numbers stay attached to logical pages after removal.
    const auto pdf = directory.filePath("scanning-source.pdf");
    {
        QPdfWriter writer(pdf);
        writer.setResolution(150);
        writer.setPageSize(QPageSize(QSizeF(100, 120), QPageSize::Millimeter));
        writer.setPageMargins(QMarginsF(0, 0, 0, 0));
        QPainter painter(&writer);
        painter.fillRect(0, 0, writer.width(), writer.height(), Qt::red);
        assert(writer.newPage());
        painter.fillRect(0, 0, writer.width(), writer.height(), Qt::blue);
    }
    window.loadSource(pdf);
    wait([&] { return scan->isEnabled() && pages->count() == 2; });
    pages->setCurrentRow(0);
    confirm(QMessageBox::Yes, 1);
    QTest::keyClick(pages, Qt::Key_Delete);
    wait([&] { return scan->isEnabled() && pages->count() == 1; });
    scanPage(Qt::green, {200, 120});
    assert(pages->count() == 2);
    QTimer::singleShot(0, &window, [&] {
        auto *dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget()); assert(dialog);
        dialog->selectFile(directory.filePath("mixed-pages.yagf.json"));
        QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection);
    });
    save->trigger();
    assert(ProjectFile::load(directory.filePath("mixed-pages.yagf.json"), &saved, &error));
    assert(saved.sourceForPage(1).page == 2 && saved.sourceForPage(1).path == pdf);
    blue = processing::readExportPage(saved, 1, directory, cancelled);
    assert(blue && blue->pixelColor(blue->width()/2, blue->height()/2) == QColor(Qt::blue));
    window.loadProject(directory.filePath("mixed-pages.yagf.json"));
    wait([&] { return scan->isEnabled() && pages->count() == 2; });
    pages->setCurrentRow(1);
    wait([&] { return scan->isEnabled(); });
    window.findChild<QAction *>("rawOcrOutputAction")->setChecked(true);
    auto *recognizeAll = window.findChild<QAction *>("recognizeAllAction");
    recognizeAll->trigger();
    wait([&] { return scan->isEnabled(); });
    save->trigger();
    assert(ProjectFile::load(directory.filePath("mixed-pages.yagf.json"), &saved, &error));
    assert(saved.page(1)->words.size() == 2 && saved.page(2)->words.size() == 2);
    window.findChild<QAction *>("rawOcrOutputAction")->setChecked(false);
}

static void testIntelligenceCropReview(const QTemporaryDir &directory, const QString &source)
{
    using namespace yagf;
    core::OcrDocument document;
    document.sourcePath = source;
    document.language = "eng";
    core::OcrPage page;
    page.width = 200;
    page.height = 100;
    page.language = "eng";
    page.words = { { 1, 1, 1, 1, { 30, 20, 24, 24 }, "2", "2", 80 } };
    document.replacePage(page);
    QString error;
    const auto path = directory.filePath("intelligence.yagf.json");
    assert(core::ProjectFile::save(path, document, &error));
    QFile tsv(directory.filePath("intelligence.tsv"));
    assert(tsv.open(QIODevice::WriteOnly));
    tsv.write("level\tpage_num\tblock_num\tpar_num\tline_num\tword_num\tleft\ttop\twidth\theight\tconf\ttext\n"
              "5\t1\t1\t1\t1\t1\t8\t8\t24\t24\t92\tz\n");
    tsv.close();
    const auto old = qgetenv("YAGF_TEST_TSV");
    qputenv("YAGF_TEST_TSV", QFile::encodeName(tsv.fileName()));
    ui::MainWindow window;
    window.loadProject(path);
    auto *words = window.findChild<QListWidget *>("recognizedWords");
    words->setCurrentRow(0, QItemSelectionModel::ClearAndSelect);
    auto *inspect = window.findChild<QAction *>("documentIntelligenceAction");
    assert(inspect && inspect->isEnabled());
    auto *ai = window.findChild<QAction *>("aiResultAction");
    assert(ai && ai->isEnabled());
    QTimer::singleShot(0, &window, [] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        assert(dialog && dialog->objectName() == "aiResultReview");
        assert(dialog->findChild<QPlainTextEdit *>()->toPlainText() == "2");
        dialog->reject();
    });
    ai->trigger();
    int phase = 0;
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, &window, [&] {
        auto *dialog = QApplication::activeModalWidget();
        if (!dialog)
            return;
        assert(!qobject_cast<QMessageBox *>(dialog));
        if (dialog->objectName() != "documentIntelligenceDialog")
            return;
        if (!phase) {
            auto *request = dialog->findChild<QPushButton *>("requestIntelligenceOcr");
            assert(request && request->isVisible());
            ++phase;
            request->click();
        } else {
            auto *list = dialog->findChild<QListWidget *>("intelligenceReadings");
            const auto z = list->findItems("z ", Qt::MatchStartsWith);
            assert(z.size() == 1);
            list->setCurrentItem(z.first());
            ++phase;
            poll.stop();
            dialog->findChild<QPushButton *>("useIntelligenceReading")->click();
        }
    });
    poll.start(15);
    inspect->trigger();
    qputenv("YAGF_TEST_TSV", old);
    assert(phase == 2);
    QAction *save = nullptr, *undo = nullptr;
    for (auto *action : window.findChildren<QAction *>()) {
        if (action->shortcut() == QKeySequence::Save)
            save = action;
        if (action->shortcut() == QKeySequence::Undo)
            undo = action;
    }
    assert(save && undo);
    save->trigger();
    core::OcrDocument saved;
    assert(core::ProjectFile::load(path, &saved, &error));
    const auto *fresh = saved.page(1);
    assert(fresh && fresh->words.first().text == "z" && fresh->words.first().originalText == "2");
    assert(fresh->recognitionHistory.runs.size() == 1 && fresh->recognitionHistory.decisions.size() == 1);
    assert(fresh->recognitionHistory.decisions.first().runId == fresh->recognitionHistory.runs.first().id);
    undo->trigger();
    save->trigger();
    assert(core::ProjectFile::load(path, &saved, &error));
    assert(saved.page(1)->words.first().text == "2" && saved.page(1)->ocrReadings.size() == 1);
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QTemporaryDir directory;
    assert(directory.isValid());
    qputenv("XDG_DATA_HOME", QFile::encodeName(directory.path()));
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, directory.path());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
    {
        processing::OcrService text; text.provider="tesseract";text.label="Local Russian text";text.language="rus";
        processing::OcrService formulas;formulas.provider="paddleocr-vl";formulas.label="Jetson formulas";
        formulas.url=QUrl("http://127.0.0.1:9");formulas.model="PaddleOCR-VL-1.6";formulas.language="auto";
        QSettings preferences("YAGF","YAGF Revival");
        preferences.setValue("ocr/primaryService",QJsonDocument(processing::ocrServicesJson({text}).first().toObject()).toJson(QJsonDocument::Compact));
        preferences.setValue("ocr/formulaService",QJsonDocument(processing::ocrServicesJson({formulas}).first().toObject()).toJson(QJsonDocument::Compact));
        preferences.setValue("ocr/serviceLibrary",QJsonDocument(processing::ocrServicesJson({formulas})).toJson(QJsonDocument::Compact));
        for (int reopen=0;reopen<2;++reopen) {
            yagf::ui::MainWindow settingsWindow;
            assert(settingsWindow.findChild<QLineEdit *>("languageField")->text()=="rus");
            auto *settings=settingsWindow.findChild<QAction *>("ocrServicesAction");assert(settings);
            QTimer::singleShot(0,&settingsWindow,[&,reopen] {
                auto *dialog=qobject_cast<QDialog *>(QApplication::activeModalWidget());assert(dialog);
                assert(dialog->windowTitle()=="Recognition settings");
                auto *formula=dialog->findChild<QComboBox *>("formulaOcrEngine");assert(formula);
                assert(formula->currentData().toString()==formulas.key());
                assert(dialog->findChild<QComboBox *>("textOcrEngine")->currentData().toString()==text.key());
                auto *layout=dialog->findChild<QLineEdit *>("pageLayoutServiceUrl");assert(layout);
                if (reopen) {assert(layout->text()=="http://127.0.0.1:9871");assert(dialog->findChild<QCheckBox *>("automaticPageLayout")->isChecked());dialog->reject();}
                else {layout->setText("http://127.0.0.1:9871");dialog->findChild<QCheckBox *>("automaticPageLayout")->setChecked(true);dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();}
            });
            settings->trigger();
            preferences.sync();assert(preferences.status()==QSettings::NoError);
        }
        for (const auto *key:{"ocr/primaryService","ocr/formulaService","ocr/serviceLibrary","ocr/discoveryHosts","layout/url","layout/threshold","layout/automatic"}) preferences.remove(key);
        QSettings().remove("layout/url");QSettings().remove("layout/threshold");
    }
    const QString sourcePath = directory.filePath(QStringLiteral("source.png"));
    QImage image(200, 100, QImage::Format_RGB32);
    image.fill(Qt::white);
    assert(image.save(sourcePath));
    const QString fixturePath = directory.filePath(QStringLiteral("result.tsv"));
    QFile fixture(fixturePath);
    assert(fixture.open(QIODevice::WriteOnly));
    const QByteArray tsv =
        "level\tpage_num\tblock_num\tpar_num\tline_num\tword_num\tleft\ttop\twidth\theight\tconf\ttext\n"
        "5\t1\t1\t1\t1\t1\t10\t20\t30\t12\t90\tmod\n"
        "5\t1\t1\t1\t1\t2\t41\t20\t70\t12\t90\ternization\n";
    assert(fixture.write(tsv) == tsv.size());
    fixture.close();
    const QString scriptPath = directory.filePath(QStringLiteral("tesseract"));
    const QString argsPath = directory.filePath(QStringLiteral("tesseract-args.txt"));
    QFile script(scriptPath);
    assert(script.open(QIODevice::WriteOnly));
    const QByteArray body = "#!/bin/sh\n"
        "if [ \"$1\" = \"--list-langs\" ]; then printf 'List of available languages:\\neng\\ndeu\\n'; exit 0; fi\n"
        "echo run >> \"$YAGF_TEST_ARGS.runs\"\n"
        "printf '%s\\n' \"$@\" > \"$YAGF_TEST_ARGS\"\ncat \"$YAGF_TEST_TSV\"\n";
    assert(script.write(body) == body.size());
    script.close();
    assert(script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    qputenv("PATH", QFile::encodeName(directory.path()) + ':' + qgetenv("PATH"));
    qputenv("YAGF_TEST_TSV", QFile::encodeName(fixturePath));
    qputenv("YAGF_TEST_ARGS", QFile::encodeName(argsPath));

    testIntelligenceCropReview(directory, sourcePath);
    testInlineTarget(directory, sourcePath);
    testInlineTarget(directory, sourcePath, true);
    if (app.arguments().contains("--inline-target-only")) return 0;
    testUnrecognizedProjectSaving(directory);
    if (app.arguments().contains("--unrecognized-save-only")) return 0;
    testScannedPages(directory);
    if (app.arguments().contains("--scanned-pages-only")) return 0;

    yagf::ui::MainWindow window;
    auto *rawOcr = window.findChild<QAction *>("rawOcrOutputAction");
    assert(rawOcr && rawOcr->isCheckable() && !rawOcr->isChecked());
    auto *cancelRecognition = window.findChild<QAction *>("cancelRecognitionAction");
    assert(cancelRecognition && !cancelRecognition->isVisible());
    auto *initialSkew = window.findChild<QAction *>("correctSkewAction");
    assert(initialSkew && !initialSkew->isEnabled() && !initialSkew->icon().isNull());
    auto *initialUndoSkew = window.findChild<QAction *>("undoSkewCorrectionAction");
    assert(initialUndoSkew && !initialUndoSkew->isVisible() && !initialUndoSkew->isEnabled());
    auto matchOption = [](const yagf::ui::MainWindow &window) -> QAction * {
        for (auto *action : window.findChildren<QAction *>())
            if (action->text() == QStringLiteral("Match similar symbol images")) return action;
        return nullptr;
    };
    auto *similar = matchOption(window);
    assert(similar && similar->isCheckable() && !similar->isChecked());
    window.loadSource(sourcePath);
    auto *words = window.findChild<QListWidget *>(QStringLiteral("recognizedWords"));
    auto *editor = window.findChild<QTextEdit *>(QStringLiteral("recognizedTextEditor"));
    assert(words && editor);
    // Export a fresh scan before OCR produces any page records.
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    const QStringList titles{"Current page as image…", "Current page as image…", "All pages as PDF…"};
    const QStringList suffixes{"png", "jpg", "pdf"};
    for (int i = 0; i < titles.size(); ++i) {
        QAction *action = nullptr;
        for (auto *candidate : window.findChildren<QAction *>())
            if (candidate->text() == titles[i]) action = candidate;
        assert(action && action->isEnabled());
        auto *group = qobject_cast<QMenu *>(action->parent());
        assert(group && group->title() == "Scans");
        auto *exports = qobject_cast<QMenu *>(group->parent());
        assert(exports && exports->title() == "Export");
        QStringList groups;
        for (auto *entry : exports->actions()) groups.append(entry->text());
        assert((groups == QStringList{"Scans", "PDF", "Word document (.docx)…", "Text and HTML", "OCR data"}));
        const QString stem = directory.filePath("scan");
        const QString destination = stem + "." + suffixes[i];
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
            assert(dialog);
            if (i < 2) {
                assert(dialog->nameFilters().size() == 2);
                dialog->selectNameFilter(dialog->nameFilters().at(i));
            }
            dialog->selectFile(i < 2 ? stem : destination);
            QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection);
        });
        action->trigger();
        QElapsedTimer wait;
        wait.start();
        auto *ocrButton = window.findChild<QToolButton *>("primaryButton");
        assert(ocrButton);
        while ((!QFileInfo::exists(destination) || !ocrButton->isEnabled()) && wait.elapsed() < 5000) {
            QApplication::processEvents();
            QThread::msleep(5);
        }
        QApplication::processEvents();
        if (i < 2) {
            QImageReader reader(destination);
            assert(reader.format().toLower() == (i == 0 ? "png" : "jpeg"));
            assert(reader.read().size() == image.size());
        } else {
            QFile pdf(destination);
            assert(pdf.open(QIODevice::ReadOnly) && pdf.read(5) == "%PDF-");
        }
    }
    auto *restorePage = window.findChild<QAction *>("restorePageAction");
    assert(restorePage && restorePage->isEnabled());
    QTimer::singleShot(0, &window, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        assert(dialog);
        auto *canvas = dialog->findChild<QWidget *>("restorationCanvas");
        auto *tool = dialog->findChild<QComboBox *>("restorationTool");
        auto *apply = dialog->findChild<QPushButton *>("applyRegion");
        auto *ok = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
        assert(tool && canvas && apply && !ok->isEnabled());
        tool->setCurrentIndex(1);
        const auto center = canvas->rect().center();
        QTest::mousePress(canvas, Qt::LeftButton, {}, center-QPoint(30, 15));
        QTest::mouseRelease(canvas, Qt::LeftButton, {}, center+QPoint(30, 15));
        assert(apply->isEnabled());
        apply->click();
        QElapsedTimer timer;
        timer.start();
        while (!ok->isEnabled() && timer.elapsed() < 5000) QTest::qWait(10);
        assert(ok->isEnabled());
        ok->click();
    });
    restorePage->trigger();
    // The edit uses the same page-level undo history as the OCR editor.
    for (auto *action : window.findChildren<QAction *>()) {
        if (action->shortcut() == QKeySequence::Undo) {
            assert(action->isEnabled());
            action->trigger();
            break;
        }
    }
    auto *recognize = window.findChild<QToolButton *>("primaryButton");
    QPushButton *apply = nullptr;
    for (QPushButton *button : window.findChildren<QPushButton *>()) {
        if (button->text() == QStringLiteral("Apply correction")) apply = button;
    }
    assert(recognize && apply && recognize->isEnabled());
    assert(!recognize->icon().isNull());
    assert(recognize->popupMode() == QToolButton::MenuButtonPopup);
    auto *recognitionMenu = recognize->menu();
    auto *mathOnly = window.findChild<QAction *>("mathOcrMode");
    auto *recognizeAllAction = window.findChild<QAction *>("recognizeAllAction");
    assert(recognitionMenu && mathOnly && mathOnly->isCheckable());
    assert(recognitionMenu->actions().contains(recognize->defaultAction()) && recognitionMenu->actions().contains(recognizeAllAction));
    assert(recognitionMenu->actions().contains(mathOnly));
    assert(recognize->icon().pixmap(24,24).toImage() != recognizeAllAction->icon().pixmap(24,24).toImage());
    mathOnly->setChecked(true);
    assert(recognize->text() == "Recognize math");
    mathOnly->setChecked(false);
    auto *spellCheck = window.findChild<QAction *>("spellCheckAction");
    auto *spellingLanguage = window.findChild<QComboBox *>("spellLanguage");
    auto *editorSplit = window.findChild<QSplitter *>("textEditorSplitter");
    assert(spellCheck && spellCheck->isChecked() && spellingLanguage && editorSplit && editorSplit->count() == 2);
    assert(editor->minimumHeight() == 120);
    auto *language = window.findChild<QLineEdit *>(QStringLiteral("languageField"));
    QAction *picker = nullptr;
    for (QAction *action : window.findChildren<QAction *>())
        if (action->text() == QStringLiteral("Choose OCR languages…")) picker = action;
    assert(language && picker);
    bool choseLanguages = false;
    QTimer::singleShot(0, &window, [&] {
        auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        assert(menu && menu->objectName() == "ocrLanguageMenu");
        auto *list = menu->findChild<QListWidget *>(QStringLiteral("availableOcrLanguages"));
        assert(list && list->count() == 2);
        for (int i = 0; i < list->count(); ++i)
            if (list->item(i)->text() == QStringLiteral("deu")) list->item(i)->setCheckState(Qt::Checked);
        choseLanguages = true;
        menu->close();
    });
    picker->trigger();
    assert(choseLanguages && language->text() == QStringLiteral("eng+deu"));
    auto *languageDropdown = window.findChild<QToolButton *>("languagePickerButton");
    assert(languageDropdown->popupMode() == QToolButton::InstantPopup);
    assert(languageDropdown->text().contains("eng+deu") && !language->isVisible());
    QAction *undo = nullptr, *redo = nullptr;
    for (QAction *action : window.findChildren<QAction *>()) {
        if (action->text() == QStringLiteral("Undo")) undo = action;
        if (action->text() == QStringLiteral("Redo")) redo = action;
    }
    assert(undo && redo);
    auto *recognizeWholeDocument = window.findChild<QAction *>("recognizeAllAction");
    assert(recognizeWholeDocument && recognizeWholeDocument->isEnabled());
    recognizeWholeDocument->trigger();
    assert(cancelRecognition->isVisible() && window.findChild<QPushButton *>("secondaryButton")->isEnabled());
    assert(!window.findChild<QAction *>("cancelExportAction")->isVisible());
    auto *singleProgress = window.findChild<QProgressDialog *>("recognitionProgressDialog");
    assert(singleProgress && singleProgress->isVisible());
    assert(singleProgress->value() == 0 && singleProgress->maximum() == 1);
    assert(singleProgress->labelText() == "0 / 1 pages recognized");
    assert(!rawOcr->isEnabled());
    QElapsedTimer timer;
    timer.start();
    while (words->count() != 2 && timer.elapsed() < 3000) {
        QApplication::processEvents();
        QThread::msleep(10);
    }
    assert(words->count() == 2);
    assert(!singleProgress->isVisible());
    assert(!cancelRecognition->isVisible());
    QFile args(argsPath);
    assert(args.open(QIODevice::ReadOnly));
    assert(args.readAll().split('\n').contains("eng+deu"));
    {
        yagf::ui::MainWindow rawSwitchWindow;
        rawSwitchWindow.loadSource(sourcePath);
        auto *rawMode = rawSwitchWindow.findChild<QAction *>("rawOcrOutputAction");
        auto *recognizeAll = rawSwitchWindow.findChild<QAction *>("recognizeAllAction");
        auto *rawBlocks = rawSwitchWindow.findChild<QListWidget *>("imageRegions");
        auto *rawWords = rawSwitchWindow.findChild<QListWidget *>("recognizedWords");
        for (bool raw : {false, true, false}) {
            rawMode->setChecked(raw);
            recognizeAll->trigger();
            timer.restart();
            while (!recognizeAll->isEnabled() && timer.elapsed() < 3000) {
                QApplication::processEvents();
                QThread::msleep(5);
            }
            assert(recognizeAll->isEnabled() && rawWords->count() == 2);
            assert(rawBlocks->count() == (raw ? 0 : 1));
        }
    }
    auto *review = window.findChild<QPushButton *>(QStringLiteral("reviewWordButton"));
    assert(review);
    words->setCurrentRow(0);
    QApplication::processEvents();
    assert(review->isEnabled());
    bool usedReview = false;
    QTimer::singleShot(0, &window, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        assert(dialog);
        auto *verified = dialog->findChild<QLineEdit *>(QStringLiteral("verifiedCorrection"));
        assert(verified);
        verified->setText(QStringLiteral("modern"));
        usedReview = true;
        dialog->accept();
    });
    review->click();
    assert(usedReview && words->item(0)->text() == QStringLiteral("modern"));
    undo->trigger();
    assert(words->item(0)->text() == QStringLiteral("mod"));
    assert(editor->toPlainText() == QStringLiteral("mod ernization"));

    editor->setPlainText(QStringLiteral("modernization"));
    similar->trigger();
    assert(similar->isChecked() && editor->toPlainText() == QStringLiteral("modernization"));
    apply->click();
    assert(words->count() == 1 && words->item(0)->text() == QStringLiteral("modernization"));
    assert(undo->isEnabled());
    undo->trigger();
    assert(words->count() == 2 && words->item(0)->text() == QStringLiteral("mod"));
    assert(redo->isEnabled());
    redo->trigger();
    assert(words->count() == 1);

    editor->setPlainText(QStringLiteral("modern ization"));
    apply->click();
    assert(words->count() == 2);
    undo->trigger();
    assert(words->count() == 1);

    editor->setPlainText(QStringLiteral("modernization now"));
    apply->click();
    assert(words->count() == 2 && words->item(1)->text() == QStringLiteral("now"));
    undo->trigger();
    assert(words->count() == 1);

    editor->clear();
    apply->click();
    assert(words->count() == 0);
    undo->trigger();
    assert(words->count() == 1 && words->item(0)->text() == QStringLiteral("modernization"));
    undo->trigger();
    assert(words->count() == 2 && words->item(0)->text() == QStringLiteral("mod"));
    undo->trigger();
    assert(words->count() == 0);
    redo->trigger();
    assert(words->count() == 2);

    QAction *engineAction = nullptr;
    for (QAction *action : window.findChildren<QAction *>())
        if (action->text() == QStringLiteral("OCR engine…")) engineAction = action;
    assert(engineAction);
    bool selectedDocTr = false;
    QTimer::singleShot(0, &window, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        assert(dialog);
        auto *choice = dialog->findChild<QComboBox *>();
        assert(choice && choice->count() == 7);
        choice->setCurrentIndex(2);
        auto *address = dialog->findChild<QLineEdit *>();
        assert(address);
        address->setText(QStringLiteral("http://127.0.0.1:8766"));
        selectedDocTr = true;
        dialog->accept();
    });
    engineAction->trigger();
    assert(selectedDocTr);
    bool hasAutoLanguage = false;
    for (QLineEdit *edit : window.findChildren<QLineEdit *>())
        hasAutoLanguage |= edit->text() == QStringLiteral("auto");
    assert(hasAutoLanguage);

    yagf::ui::MainWindow regionWindow;
    auto *savedOption = matchOption(regionWindow);
    assert(savedOption && savedOption->isChecked());
    savedOption->trigger();
    regionWindow.loadSource(sourcePath);
    regionWindow.show();
    QApplication::processEvents();
    auto *toolbar = regionWindow.findChild<QToolBar *>("mainToolbar");
    assert(toolbar);
    QAction *scan = nullptr;
    for (auto *action : toolbar->actions()) {
        assert(action->text() != "Pan" && action->text() != "Select region");
        if (action->iconText() == "Scan") scan = action;
    }
    assert(scan && scan->isEnabled());
    QTimer::singleShot(0, &regionWindow, [] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        assert(dialog && dialog->windowTitle() == "Scan a page");
        dialog->reject();
    });
    scan->trigger();
    const auto separatorWidths = [&] {
        QVector<int> widths;
        for (auto *toolbar : regionWindow.findChildren<QToolBar *>())
            for (auto *action : toolbar->actions()) if (action->isSeparator()) {
                auto *separator = toolbar->widgetForAction(action);
                if (!separator->isVisible()) continue;
                assert(separator->width() == separator->sizeHint().width());
                widths.append(separator->width());
            }
        assert(!widths.isEmpty());
        return widths;
    };
    const QSize startupSize = regionWindow.size();
    const auto startupSeparators = separatorWidths();
    regionWindow.resize(startupSize + QSize(90, 30));
    QApplication::processEvents();
    separatorWidths();
    regionWindow.resize(startupSize);
    QApplication::processEvents();
    assert(separatorWidths() == startupSeparators);
    const auto dragDivider = [&](const char *name, int delta) {
        auto *divider = regionWindow.findChild<QSplitter *>(name);
        assert(divider && divider->orientation() == Qt::Vertical && divider->count() == 2);
        const auto before = divider->sizes();
        auto *handle = divider->handle(1);
        assert(handle->isVisible() && handle->height() >= 8);
        const QPoint from = handle->rect().center();
        const QPoint to = from + QPoint(0, delta);
        QTest::mousePress(handle, Qt::LeftButton, {}, from);
        QTest::mouseMove(handle, to);
        QTest::mouseRelease(handle, Qt::LeftButton, {}, to);
        QApplication::processEvents();
        const auto after = divider->sizes();
        assert(delta > 0 ? after[0] > before[0] : after[0] < before[0]);
        assert(delta > 0 ? after[1] < before[1] : after[1] > before[1]);
        divider->setSizes(before);
        QApplication::processEvents();
    };
    dragDivider("navigationSplitter", 50);
    dragDivider("textEditorSplitter", -70);
    auto *pageView = regionWindow.findChild<yagf::ui::PageView *>(QStringLiteral("pagePreview"));
    auto *regions = regionWindow.findChild<QListWidget *>(QStringLiteral("imageRegions"));
    auto *regionWords = regionWindow.findChild<QListWidget *>(QStringLiteral("recognizedWords"));
    assert(pageView && regions && regionWords);
    auto *imageTools = pageView->findChild<QToolBar *>("imageToolbar");
    auto *pinTools = pageView->findChild<QAction *>("pinImageToolbarAction");
    auto *rotateRight = pageView->findChild<QAction *>("rotateImageRightAction");
    auto *rotateLeft = pageView->findChild<QAction *>("rotateImageLeftAction");
    auto *correctSkew = pageView->findChild<QAction *>("correctSkewAction");
    auto *blockRecognize = pageView->findChild<QAction *>("imageRecognizeBlockAction");
    assert(imageTools && pinTools && rotateRight && rotateLeft && blockRecognize);
    assert(correctSkew && correctSkew->isVisible() && correctSkew->isEnabled());
    assert(imageTools->actions().first() == pinTools);
    // Moving from block resize/drawing mode to overlay controls uses the standard arrow.
    pinTools->setChecked(true);
    pageView->viewport()->setCursor(Qt::SizeHorCursor);
    auto *splitControl=qobject_cast<QToolButton *>(imageTools->widgetForAction(pageView->findChild<QAction *>("imageSmartSplitAction")));
    assert(splitControl);
    QTest::mouseMove(splitControl,splitControl->rect().center());
    QApplication::processEvents();
    assert(splitControl->cursor().shape()==Qt::ArrowCursor);
    assert(pageView->viewport()->cursor().shape()==Qt::ArrowCursor);

    if (qEnvironmentVariableIsSet("YAGF_CAPTURE_IMAGE_TOOLBAR")) {
        pinTools->setChecked(true);
        QTest::mouseMove(pageView->viewport(), QPoint(5, pageView->viewport()->height()-10));
        QApplication::processEvents();
        assert(imageTools->grab().save("/tmp/yagf-image-toolbar-idle.png"));
        auto *split=pageView->findChild<QAction *>("imageSmartSplitAction");
        auto *button=qobject_cast<QToolButton *>(imageTools->widgetForAction(split)); assert(button);
        QTest::mouseMove(button,QPoint(button->width()-7,button->height()/2));
        QApplication::processEvents();
        assert(imageTools->grab().save("/tmp/yagf-image-toolbar-hover.png"));
    }

    assert(imageTools->parentWidget() == pageView->viewport());
    for (auto *action : toolbar->actions()) assert(action->text() != "Zoom in" && action->text() != "Zoom out" && action->text() != "Fit page");
    pinTools->setChecked(true);
    assert(imageTools->isVisible());
    pinTools->setChecked(false);
    QTest::mouseMove(pageView->viewport(), QPoint(pageView->viewport()->width()/2, pageView->viewport()->height()-10));
    QTest::qWait(250);
    assert(!imageTools->isVisible());
    QTest::mouseMove(pageView->viewport(), QPoint(5, 5));
    assert(imageTools->isVisible());
    pinTools->setChecked(true);
    QTest::mouseMove(pageView->viewport(), QPoint(5, pageView->viewport()->height()-10));
    assert(imageTools->isVisible());
    const qreal originalZoom = pageView->transform().m11();
    pageView->findChild<QAction *>("zoomInAction")->trigger();
    assert(pageView->transform().m11() > originalZoom);
    pageView->findChild<QAction *>("fitPageAction")->trigger();
    // The overlay stays at the top of the viewport while the image moves.
    const QRectF originalSceneRect = pageView->scene()->sceneRect();
    pageView->zoomBy(1);
    pageView->scene()->setSceneRect(0, 0, 1000, 1600);
    QApplication::processEvents();
    assert(pageView->verticalScrollBar()->maximum() > 500 && pageView->horizontalScrollBar()->maximum() > 250);
    pinTools->setChecked(false);
    QTest::mouseMove(pageView->viewport(), QPoint(5, pageView->viewport()->height()-10));
    pageView->verticalScrollBar()->setValue(500);
    pageView->horizontalScrollBar()->setValue(250);
    QApplication::processEvents();
    assert(!imageTools->isVisible() && imageTools->y() == 10);
    QTest::mouseMove(pageView->viewport(), QPoint(5, 5));
    assert(imageTools->isVisible() && imageTools->y() == 10);
    pageView->verticalScrollBar()->setValue(700);
    pageView->horizontalScrollBar()->setValue(350);
    assert(imageTools->isVisible() && imageTools->y() == 10);
    assert(imageTools->x() == std::max(0, (pageView->viewport()->width()-imageTools->width())/2));
    QWheelEvent wheel(QPointF(5,5), QPointF(pageView->viewport()->mapToGlobal(QPoint(5,5))),
                      QPoint(), QPoint(0,-120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(pageView->viewport(), &wheel);
    assert(imageTools->isVisible() && imageTools->y() == 10);
    pinTools->setChecked(true);
    QTest::mouseMove(pageView->viewport(), QPoint(5, pageView->viewport()->height()-10));
    pageView->verticalScrollBar()->setValue(900);
    assert(imageTools->isVisible() && imageTools->y() == 10);
    pageView->scene()->setSceneRect(originalSceneRect);
    pageView->fitPage();
    rotateRight->trigger();
    assert(pageView->scene()->sceneRect().size() == QSizeF(100, 200));
    for (auto *action : regionWindow.findChildren<QAction *>()) if (action->shortcut() == QKeySequence::Undo) action->trigger();
    assert(pageView->scene()->sceneRect().size() == QSizeF(200, 100));
    const auto drag = [&](QPointF start, QPointF end) {
        const QPoint from = pageView->mapFromScene(start);
        const QPoint to = pageView->mapFromScene(end);
        QTest::mousePress(pageView->viewport(), Qt::LeftButton, {}, from);
        QTest::mouseMove(pageView->viewport(), to);
        QTest::mouseRelease(pageView->viewport(), Qt::LeftButton, {}, to);
        QApplication::processEvents();
    };
    const auto regionBox = [&](int index) {
        for (QGraphicsItem *item : pageView->scene()->items())
            if (auto *rect = qgraphicsitem_cast<QGraphicsRectItem *>(item);
                rect && rect->data(1).isValid() && rect->data(1).toInt() == index)
                return rect->rect().toAlignedRect();
        return QRect{};
    };
    drag({5, 5}, {90, 75});
    drag({105, 5}, {195, 75});
    assert(regions->count() == 2);
    assert(!regions->item(0)->icon().isNull() && !regions->item(1)->icon().isNull());
    assert(std::abs(regionBox(0).x() - 5) <= 2 && std::abs(regionBox(0).width() - 85) <= 3);
    assert(std::abs(regionBox(1).x() - 105) <= 2 && std::abs(regionBox(1).width() - 90) <= 3);
    const QRect original = regionBox(0);
    drag({30, 40}, {40, 45});
    assert(regionBox(0).left() > original.left() && regionBox(0).top() > original.top());
    const QRect moved = regionBox(0);
    drag({qreal(moved.right()), qreal(moved.center().y())},
         {moved.right() - 10.0, qreal(moved.center().y())});
    assert(regionBox(0).width() < moved.width());
    regions->setCurrentRow(0);
    assert(!correctSkew->isVisible());
    pageView->setFocus();
    QTest::keyClick(pageView, Qt::Key_Delete);
    assert(regions->count() == 1);
    QAction *regionUndo = nullptr, *regionRedo = nullptr;
    for (QAction *action : regionWindow.findChildren<QAction *>())
        if (action->text() == QStringLiteral("Undo")) regionUndo = action;
        else if (action->text() == QStringLiteral("Redo")) regionRedo = action;
    assert(regionUndo && regionRedo);
    regionUndo->trigger();
    assert(regions->count() == 2);
    regionUndo->trigger();
    assert(regionBox(0).width() == moved.width());
    regionUndo->trigger();
    assert(std::abs(regionBox(0).x() - 5) <= 2);
    regionRedo->trigger();
    regionRedo->trigger();
    const QRect beforeCollision = regionBox(0);
    const QPoint resizeStart = pageView->mapFromScene(beforeCollision.right(), beforeCollision.center().y());
    const QPoint resizeEnd = pageView->mapFromScene(180, beforeCollision.center().y());
    QTest::mousePress(pageView->viewport(), Qt::LeftButton, {}, resizeStart);
    QTest::mouseMove(pageView->viewport(), resizeEnd);
    QApplication::processEvents();
    const QRect allowedResize = regionBox(0);
    QTest::mouseRelease(pageView->viewport(), Qt::LeftButton, {}, resizeEnd);
    QApplication::processEvents();
    assert(regionBox(0) == allowedResize);
    assert(regionBox(0).intersects(regionBox(1)));
    assert(!pageView->pageInk().conflicts(regionBox(0), regionBox(1)));
    regionUndo->trigger();
    assert(regionBox(0) == beforeCollision);
    drag({qreal(beforeCollision.center().x()), qreal(beforeCollision.center().y())},
         {180, qreal(beforeCollision.center().y())});
    assert(regionBox(0).intersects(regionBox(1)));
    assert(!pageView->pageInk().conflicts(regionBox(0), regionBox(1)));
    regionUndo->trigger();
    drag({95, 0}, {195, 80});
    assert(regions->count() == 3); // The new block keeps the permitted part of the drag.
    assert(!pageView->pageInk().conflicts(regionBox(2), regionBox(0)) && !pageView->pageInk().conflicts(regionBox(2), regionBox(1)));
    regionUndo->trigger();
    assert(regions->count() == 2);

    auto blockMenu = [&](int index, const QString &choice, bool miniature = false, bool keyboard = false) {
        QTimer::singleShot(0, &regionWindow, [choice] {
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            assert(menu && menu->objectName() == "blockContextMenu");
            assert(menu->actions().first()->text() == "Recognize block");
            assert(!menu->actions().first()->icon().isNull() && menu->actions().first()->isIconVisibleInMenu());
            assert(!menu->actions().last()->icon().isNull() && menu->actions().last()->isIconVisibleInMenu());
            assert(menu->actions().at(1)->text() == "Select block language");
            if (choice == "Check disabled") {
                assert(!menu->actions().first()->isEnabled());
                menu->close();
                return;
            }
            QAction *selected = nullptr;
            if (choice == "Remove block") selected = menu->actions().last();
            else if (choice == "Recognize block") selected = menu->actions().first();
            else if (choice == "Clear all blocks") {
                for (auto *action : menu->actions()) if (action->text() == choice) selected = action;
            }
            else for (auto *action : menu->actions().at(1)->menu()->actions())
                if (action->text() == choice) selected = action;
            assert(selected);
            selected->trigger();
            menu->close();
        });
        if (miniature) {
            if (keyboard) regions->setCurrentRow(index);
            regions->scrollToItem(regions->item(index));
            regions->doItemsLayout();
            const QRect bounds = regions->visualItemRect(regions->item(index));
            const QPoint point(regions->viewport()->width() - 3, bounds.center().y());
            QContextMenuEvent event(keyboard ? QContextMenuEvent::Keyboard : QContextMenuEvent::Mouse,
                                   point, regions->viewport()->mapToGlobal(point));
            QApplication::sendEvent(regions->viewport(), &event);
        } else {
            const QPoint point = pageView->mapFromScene(regionBox(index).center());
            QContextMenuEvent event(QContextMenuEvent::Mouse, point, pageView->viewport()->mapToGlobal(point));
            QApplication::sendEvent(pageView->viewport(), &event);
        }
    };
    regions->setCurrentRow(0);
    blockMenu(1, "German", true);
    assert(regions->item(1)->text().contains("deu"));
    assert(regions->currentRow() == 1 && !regions->item(0)->text().contains("deu"));
    auto *regionSpelling = regionWindow.findChild<QComboBox *>("spellLanguage");
    assert(regionSpelling);
    if (regionSpelling->findData("de_DE") >= 0) assert(regionSpelling->toolTip().contains("de_DE"));
    auto *recognizeRegions = regionWindow.findChild<QToolButton *>("primaryButton");
    assert(recognizeRegions);
    const auto invocations = [&] {
        QFile log(argsPath + ".runs");
        assert(log.open(QIODevice::ReadOnly));
        return log.readAll().count('\n');
    };
    const auto recognizeSelection = [&](int expectedRuns) {
        const auto previous = invocations();
        recognizeRegions->click();
        timer.restart();
        while (!recognizeRegions->isEnabled() && timer.elapsed() < 5000) QTest::qWait(5);
        assert(recognizeRegions->isEnabled() && invocations() == previous + expectedRuns);
    };
    assert(regions->selectedItems().size() == 1 && regions->item(1)->isSelected());
    recognizeSelection(1);
    assert(regionWords->count() == 1);
    QTest::mouseClick(pageView->viewport(), Qt::LeftButton, Qt::ShiftModifier,
                      pageView->mapFromScene(regionBox(0).center()));
    assert(regions->selectedItems().size() == 2);
    QTest::mouseClick(pageView->viewport(), Qt::LeftButton, {}, pageView->mapFromScene(50, 95));
    assert(regions->selectedItems().isEmpty() && regions->count() == 2);
    recognizeSelection(1);
    assert(regionWords->count() == 2 && regions->selectedItems().isEmpty());
    regions->setCurrentRow(0, QItemSelectionModel::ClearAndSelect);
    QTest::mouseClick(regions->viewport(), Qt::LeftButton, Qt::ControlModifier,
                      regions->visualItemRect(regions->item(1)).center());
    assert(regions->selectedItems().size() == 2);
    recognizeSelection(2);
    assert(regionWords->count() == 2 && regions->selectedItems().size() == 2);
    pageView->zoomBy(2);
    auto *scroll = pageView->horizontalScrollBar();
    scroll->setValue(scroll->maximum() / 2);
    const int beforePan = scroll->value();
    const QPoint center = pageView->viewport()->rect().center();
    QTest::mousePress(pageView->viewport(), Qt::MiddleButton, {}, center);
    QTest::mouseMove(pageView->viewport(), center + QPoint(15, 0));
    QTest::mouseRelease(pageView->viewport(), Qt::MiddleButton, {}, center + QPoint(15, 0));
    assert(scroll->value() < beforePan && regions->selectedItems().size() == 2);
    pageView->fitPage();
    QTest::keyClick(pageView, Qt::Key_Escape);
    assert(regions->selectedItems().isEmpty());
    args.close();
    assert(args.open(QIODevice::ReadOnly));
    assert(args.readAll().split('\n').contains("deu"));
    args.close();
    assert(QFile::remove(argsPath));
    blockMenu(1, "Recognize block", true);
    timer.restart();
    while (!recognizeRegions->isEnabled() && timer.elapsed() < 3000) {
        QApplication::processEvents();
        QThread::msleep(10);
    }
    assert(args.open(QIODevice::ReadOnly));
    assert(args.readAll().split('\n').contains("deu"));
    QTest::mouseClick(pageView->viewport(), Qt::LeftButton, Qt::ControlModifier,
                      pageView->mapFromScene(QPointF(30, 35)));
    auto *regionEditor = regionWindow.findChild<QTextEdit *>(QStringLiteral("recognizedTextEditor"));
    assert(regionEditor && regionEditor->toPlainText() == QStringLiteral("mod"));
    blockMenu(0, "Not a text", true, true);
    blockMenu(0, "Check disabled", true);
    assert(regions->item(0)->text().contains("Not a text"));
    assert(regionWords->count() == 1);
    recognizeRegions->click();
    timer.restart();
    while (recognizeRegions->isEnabled() == false && timer.elapsed() < 3000) {
        QApplication::processEvents();
        QThread::msleep(10);
    }
    assert(regionWords->count() == 1);
    blockMenu(1, "Not a text");
    assert(regionWords->count() == 0);
    args.close();
    assert(args.open(QIODevice::ReadOnly));
    const auto beforeSkip = args.readAll();
    recognizeRegions->click();
    QApplication::processEvents();
    args.close();
    assert(args.open(QIODevice::ReadOnly));
    assert(args.readAll() == beforeSkip); // No fallback to full-page OCR for excluded blocks.
    blockMenu(0, "Remove block", true);
    assert(regions->count() == 1);
    regionUndo->trigger();
    assert(regions->count() == 2 && regions->item(0)->text().contains("Not a text"));
    regions->setCurrentRow(0, QItemSelectionModel::ClearAndSelect);
    assert(!rotateLeft->isVisible() && !rotateRight->isVisible() && blockRecognize->isVisible());
    blockMenu(0, "Clear all blocks", true);
    assert(regions->count() == 0 && rotateLeft->isVisible() && !blockRecognize->isVisible());
    regionUndo->trigger();
    assert(regions->count() == 2);

    // Creation uses the same allowed scene rectangle for the preview and saved block,
    // even when drawing backwards or at a noninteger zoom.
    for (const auto zoom : {0.63, 1.0, 1.37}) for (const bool backwards : {false, true}) {
        yagf::ui::MainWindow clippedWindow;
        clippedWindow.loadSource(sourcePath);
        clippedWindow.show();
        QApplication::processEvents();
        auto *canvas = clippedWindow.findChild<yagf::ui::PageView *>("pagePreview");
        auto *miniatures = clippedWindow.findChild<QListWidget *>("imageRegions");
        canvas->zoomBy(zoom);
        const auto box = [&](int index) {
            for (auto *item : canvas->scene()->items())
                if (auto *rect = qgraphicsitem_cast<QGraphicsRectItem *>(item);
                    rect && rect->data(1).isValid() && rect->data(1).toInt() == index)
                    return rect->rect().toAlignedRect();
            return QRect{};
        };
        const auto press = [&](QPointF point) {
            QTest::mousePress(canvas->viewport(), Qt::LeftButton, {}, canvas->mapFromScene(point));
        };
        const auto move = [&](QPointF point) {
            QTest::mouseMove(canvas->viewport(), canvas->mapFromScene(point));
            QApplication::processEvents();
        };
        const auto release = [&](QPointF point) {
            QTest::mouseRelease(canvas->viewport(), Qt::LeftButton, {}, canvas->mapFromScene(point));
            QApplication::processEvents();
        };
        press({80, 20}); move({120, 80}); release({120, 80});
        assert(miniatures->count() == 1);
        const QRect obstacle = box(0);
        QSignalSpy created(canvas, &yagf::ui::PageView::regionSelected);
        const QPointF origin(backwards ? 190 : 10, 30), end(backwards ? 10 : 190, 70);
        press(origin); move(end);
        auto *band = canvas->viewport()->findChild<QRubberBand *>();
        assert(band && band->isVisible());
        const QRect visibleAllowed = band->geometry();
        release(end);
        assert(miniatures->count() == 2 && created.size() == 1);
        const QRect saved = box(1);
        assert(saved.intersects(obstacle) && !canvas->pageInk().conflicts(saved, obstacle));
        auto beyond = saved;
        if (backwards) beyond.setLeft(saved.left() - 1);
        else beyond.setRight(saved.right() + 1);
        assert(canvas->pageInk().conflicts(beyond, obstacle));
        assert(saved == qvariant_cast<QRectF>(created.at(0).at(0)).toAlignedRect());
        assert(visibleAllowed == canvas->mapFromScene(QRectF(saved)).boundingRect());
        QAction *undoDrawing = nullptr, *redoDrawing = nullptr;
        for (auto *action : clippedWindow.findChildren<QAction *>()) {
            if (action->text() == "Undo") undoDrawing = action;
            if (action->text() == "Redo") redoDrawing = action;
        }
        assert(undoDrawing && redoDrawing);
        undoDrawing->trigger();
        assert(miniatures->count() == 1);
        redoDrawing->trigger();
        assert(box(1) == saved);
    }
    // Opening and dismissing a book suggestion must leave the target untouched.
    yagf::core::OcrDocument book;
    book.sourcePath = sourcePath;
    yagf::core::OcrPage page;
    page.width = page.sourceWidth = image.width();
    page.height = page.sourceHeight = image.height();
    page.words.append({1, 1, 1, 1, QRect(5, 5, 40, 15), "QWldget", "QWidget", 75, true});
    page.words.last().fontFamily = "Liberation Serif";
    page.words.last().bold = true;
    page.words.append({1, 1, 1, 1, QRect(60, 5, 40, 15), "QWldget", "QWldget", 99});
    book.replacePage(page);
    const QString bookPath = directory.filePath("book.yagf.json");
    QString error;
    assert(yagf::core::ProjectFile::save(bookPath, book, &error));
    // Document context is undoable, saved with the book, and supplied to review.
    yagf::ui::MainWindow contextWindow;
    auto *contextAction = contextWindow.findChild<QAction *>("documentContextAction");
    assert(contextAction && !contextAction->isEnabled());
    contextWindow.loadProject(bookPath);
    assert(contextAction->isEnabled());
    // Recent projects persist across windows. Reopening an entry moves it to the front.
    const QString portable = directory.filePath("recent.yagfproj");
    assert(yagf::core::ProjectFile::savePortable(portable, book, &error));
    {
        yagf::ui::MainWindow other;
        other.loadProject(portable);
    }
    auto *recent = contextWindow.findChild<QMenu *>("recentProjectsMenu");
    assert(recent);
    auto *fileMenu = qobject_cast<QMenu *>(recent->parent());
    assert(fileMenu);
    fileMenu->aboutToShow();
    assert(recent->actions()[0]->data().toString() == portable);
    assert(recent->actions()[1]->data().toString() == bookPath);
    recent->actions()[1]->trigger();
    fileMenu->aboutToShow();
    assert(recent->actions()[0]->data().toString() == bookPath);
    assert(recent->actions()[1]->data().toString() == portable);
    recent->actions().last()->trigger();
    fileMenu->aboutToShow();
    assert(recent->actions().size() == 1 && !recent->actions()[0]->isEnabled());

    auto *preserveFormat = contextWindow.findChild<QAction *>("preserveFormattingAction");
    assert(preserveFormat && preserveFormat->isEnabled() && !preserveFormat->isChecked());
    auto *formattedWords = contextWindow.findChild<QListWidget *>("recognizedWords");
    auto *formattedEditor = contextWindow.findChild<QTextEdit *>("recognizedTextEditor");
    formattedWords->setCurrentRow(0, QItemSelectionModel::ClearAndSelect);
    assert(formattedEditor->currentCharFormat().fontFamilies().toStringList().contains("Liberation Serif"));
    assert(formattedEditor->currentCharFormat().fontWeight() == QFont::Bold);
    auto *spellToggle = contextWindow.findChild<QAction *>("spellCheckAction");
    auto *spellChoice = contextWindow.findChild<QComboBox *>("spellLanguage");
    assert(spellToggle && spellChoice);
    const bool originallyEnabled = spellToggle->isChecked();
    spellToggle->trigger();
    spellToggle->trigger();
    assert(spellToggle->isChecked() == originallyEnabled && !contextWindow.windowTitle().endsWith(" *"));
    if (spellChoice->count() > 1) {
        spellChoice->setCurrentIndex(1);
        spellChoice->setCurrentIndex(0);
        assert(!contextWindow.windowTitle().endsWith(" *"));
    }
    auto inspectContext = [&](const QString &expected, bool inference, bool change, bool accept) {
        QTimer::singleShot(0, &contextWindow, [=] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            assert(dialog && dialog->objectName() == "documentContextDialog");
            auto *description = dialog->findChild<QPlainTextEdit *>("documentDescription");
            auto *infer = dialog->findChild<QCheckBox *>("inferDocumentContext");
            assert(description && infer && description->toPlainText() == expected && infer->isChecked() == inference);
            if (change) {
                auto *ok = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
                description->setPlainText(QString(4001, 'x'));
                assert(!ok->isEnabled());
                description->setPlainText("A Qt programming manual");
                assert(ok->isEnabled());
                infer->setChecked(false);
            }
            if (accept) dialog->accept(); else dialog->reject();
        });
        contextAction->trigger();
    };
    inspectContext({}, true, true, false);
    assert(!contextWindow.windowTitle().endsWith(" *"));
    inspectContext({}, true, true, true);
    assert(contextWindow.windowTitle().endsWith(" *"));
    QAction *contextUndo = nullptr, *contextRedo = nullptr, *contextSave = nullptr;
    for (auto *action : contextWindow.findChildren<QAction *>()) {
        if (action->text() == "Undo") contextUndo = action;
        if (action->text() == "Redo") contextRedo = action;
        if (action->text() == "Save project") contextSave = action;
    }
    assert(contextUndo && contextRedo && contextSave);
    contextUndo->trigger();
    assert(!contextWindow.windowTitle().endsWith(" *"));
    inspectContext({}, true, false, false);
    contextRedo->trigger();
    inspectContext("A Qt programming manual", false, false, false);
    auto *contextWords = contextWindow.findChild<QListWidget *>("recognizedWords");
    contextWords->setCurrentRow(1, QItemSelectionModel::ClearAndSelect);
    QTimer::singleShot(0, &contextWindow, [] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        assert(dialog);
        auto *preview = dialog->findChild<QPlainTextEdit *>("documentContextPreview");
        assert(preview && preview->toPlainText().contains("A Qt programming manual"));
        assert(!preview->toPlainText().contains("earlier text"));
        dialog->reject();
    });
    contextWindow.findChild<QPushButton *>("reviewWordButton")->click();
    preserveFormat->setChecked(true);
    auto *contextRawOcr = contextWindow.findChild<QAction *>("rawOcrOutputAction");
    assert(contextRawOcr && !contextRawOcr->isChecked());
    contextRawOcr->setChecked(true);
    contextSave->trigger();
    yagf::core::OcrDocument contextualBook;
    assert(yagf::core::ProjectFile::load(bookPath, &contextualBook, &error));
    assert(contextualBook.context.description == "A Qt programming manual");
    assert(!contextualBook.context.inferFromRecognizedText);
    assert(contextualBook.preserveFormatting);
    assert(contextualBook.useRawOcrOutput);
    yagf::ui::MainWindow rawProjectWindow;
    rawProjectWindow.loadProject(bookPath);
    assert(rawProjectWindow.findChild<QAction *>("rawOcrOutputAction")->isChecked());
    auto *formattedPdf = contextWindow.findChild<QAction *>("exportFormattedPdfAction");
    assert(formattedPdf && formattedPdf->isEnabled());
    const QString formattedPath = directory.filePath("formatted-from-ui.pdf");
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    // This fixture contains only prose. Exercise the checkbox even on machines without TeX.
    assert(QFile::link(scriptPath, directory.filePath("pdflatex")));
    QTimer::singleShot(0, &contextWindow, [&] {
        auto *dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
        assert(dialog && dialog->windowTitle() == "Export formatted PDF");
        auto *typeset = dialog->findChild<QCheckBox *>("typesetFormulas");
        assert(typeset && typeset->isVisible() && typeset->isEnabled() && !typeset->isChecked());
        typeset->setChecked(true);
        dialog->selectFile(formattedPath);
        QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection);
    });
    formattedPdf->trigger();
    timer.restart();
    while ((!QFileInfo::exists(formattedPath) || !formattedPdf->isEnabled()) && timer.elapsed() < 5000) {
        QApplication::processEvents();
        QThread::msleep(5);
    }
    QFile formattedFile(formattedPath);
    assert(formattedFile.open(QIODevice::ReadOnly) && formattedFile.read(5) == "%PDF-");
    const auto toolsPath = qgetenv("PATH");
    QTemporaryDir noTools;
    assert(noTools.isValid());
    qputenv("PATH", QFile::encodeName(noTools.path()));
    const auto withoutTex = directory.filePath("without-tex.pdf");
    QTimer::singleShot(0, &contextWindow, [&] {
        auto *dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
        assert(dialog);
        auto *typeset = dialog->findChild<QCheckBox *>("typesetFormulas");
        assert(typeset && !typeset->isEnabled() && !typeset->isChecked() && typeset->text().contains("pdfLaTeX"));
        dialog->selectFile(withoutTex);
        QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection);
    });
    formattedPdf->trigger();
    timer.restart();
    while (!formattedPdf->isEnabled() && timer.elapsed() < 5000) {
        QApplication::processEvents();
        QThread::msleep(5);
    }
    assert(formattedPdf->isEnabled() && QFileInfo(withoutTex).size() > 0);
    qputenv("PATH", toolsPath);
    for (int i = 0; i < 2; ++i) {
        QTimer::singleShot(0, &contextWindow, [] {
            auto *dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
            assert(dialog);
            auto *typeset = dialog->findChild<QCheckBox *>("typesetFormulas");
            assert(typeset && typeset->isChecked());
            typeset->setChecked(false);
            dialog->reject();
        });
        formattedPdf->trigger();
    }
    // Loading a different source resets the book-specific description and inference setting.
    contextWindow.loadSource(sourcePath);
    inspectContext({}, true, false, false);
    // Keep the original fixture for the independent correction-memory checks below.
    assert(yagf::core::ProjectFile::save(bookPath, book, &error));
    yagf::ui::MainWindow bookWindow;
    bookWindow.loadProject(bookPath);
    auto *bookWords = bookWindow.findChild<QListWidget *>("recognizedWords");
    auto *reviewBook = bookWindow.findChild<QPushButton *>("reviewWordButton");
    assert(bookWords && reviewBook && bookWords->count() == 2);
    bookWords->setCurrentRow(1, QItemSelectionModel::ClearAndSelect);
    assert(reviewBook->text().contains("QWidget"));
    for (bool accept : {false, true}) {
        QTimer::singleShot(0, &bookWindow, [accept] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            assert(dialog);
            auto *choices = dialog->findChild<QComboBox *>("bookSuggestions");
            auto *reading = dialog->findChild<QLineEdit *>("verifiedCorrection");
            auto *context = dialog->findChild<QPlainTextEdit *>("documentContextPreview");
            assert(context && context->toPlainText().contains("QWidget"));
            assert(choices && choices->count() == 1 && choices->currentIndex() == -1);
            assert(reading && reading->text() == "QWldget");
            choices->setCurrentIndex(0);
            choices->activated(0);
            assert(reading->text() == "QWidget");
            if (accept) dialog->accept();
            else dialog->reject();
        });
        reviewBook->click();
        assert(bookWords->item(1)->text() == (accept ? "QWidget" : "QWldget"));
    }
    for (auto *action : bookWindow.findChildren<QAction *>())
        if (action->text() == "Undo") action->trigger();
    assert(bookWords->item(1)->text() == "QWldget");
    bookWords->setCurrentRow(0, QItemSelectionModel::ClearAndSelect);
    auto *bookEditor = bookWindow.findChild<QTextEdit *>("recognizedTextEditor");
    auto *bookApply = bookWindow.findChild<QPushButton *>("applyButton");
    assert(bookEditor && bookApply);
    bookEditor->setPlainText("QPanel");
    bookApply->click();
    bookWords->setCurrentRow(1, QItemSelectionModel::ClearAndSelect);
    assert(reviewBook->text().contains("QPanel"));
    for (auto *action : bookWindow.findChildren<QAction *>())
        if (action->text() == "Undo") action->trigger();
    bookWords->setCurrentRow(1, QItemSelectionModel::ClearAndSelect);
    assert(reviewBook->text().contains("QWidget"));

    page.words[1].alternative = {"doctr-remote", "QWidget", {"QWldget"}, false};
    book.replacePage(page);
    assert(yagf::core::ProjectFile::save(bookPath, book, &error));
    yagf::ui::MainWindow hybrid;
    hybrid.loadProject(bookPath);
    auto *hybridWords = hybrid.findChild<QListWidget *>("recognizedWords");
    QAction *nextReading = nullptr;
    for (auto *action : hybrid.findChildren<QAction *>())
        if (action->text() == "Review next specialist reading…") nextReading = action;
    assert(hybridWords && nextReading && nextReading->isEnabled());
    for (bool accept : {false, true}) {
        QTimer::singleShot(0, &hybrid, [accept] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            assert(dialog);
            auto *copy = dialog->findChild<QPushButton *>("copySpecialist");
            auto *editor = dialog->findChild<QPlainTextEdit *>("verifiedPassage");
            assert(copy && editor && editor->toPlainText() == "QWldget");
            copy->click();
            assert(editor->toPlainText() == "QWidget");
            if (accept) dialog->accept(); else dialog->reject();
        });
        nextReading->trigger();
        assert(hybridWords->item(1)->text() == (accept ? "QWidget" : "QWldget"));
    }
    for (auto *action : hybrid.findChildren<QAction *>())
        if (action->text() == "Undo") action->trigger();
    assert(hybridWords->item(1)->text() == "QWldget");

    hybridWords->setCurrentRow(0, QItemSelectionModel::ClearAndSelect);
    hybridWords->item(1)->setSelected(true);
    auto *reviewPassage = hybrid.findChild<QPushButton *>("reviewWordButton");
    assert(reviewPassage && reviewPassage->isEnabled() && reviewPassage->text() == "Review passage…");
    QTimer::singleShot(0, &hybrid, [] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        assert(dialog && !dialog->findChild<QPushButton *>("copySpecialist"));
        auto *editor = dialog->findChild<QPlainTextEdit *>("verifiedPassage");
        assert(editor && editor->toPlainText() == "QWidget QWldget");
        editor->setPlainText("QWidget QObject");
        dialog->accept();
    });
    reviewPassage->click();
    assert(hybridWords->item(1)->text() == "QObject");

    // A structured table stays one editable block when its cells contain spaces or tabs.
    page.words[0].text = page.words[0].originalText = "Name\tValue\nRate constant\t42";
    page.words[0].layoutBlock = true;
    book.replacePage(page);
    assert(yagf::core::ProjectFile::save(bookPath, book, &error));
    yagf::ui::MainWindow scientificWindow;
    scientificWindow.loadProject(bookPath);
    auto *blocks = scientificWindow.findChild<QListWidget *>("recognizedWords");
    auto *blockEditor = scientificWindow.findChild<QTextEdit *>("recognizedTextEditor");
    auto *applyBlock = scientificWindow.findChild<QPushButton *>("applyButton");
    assert(blocks && blockEditor && applyBlock);
    blocks->setCurrentRow(0, QItemSelectionModel::ClearAndSelect);
    const QString correctedTable = "Name\tValue\nRate constant\t43";
    blockEditor->setPlainText(correctedTable);
    applyBlock->click();
    assert(blocks->count() == 2 && blocks->item(0)->text() == correctedTable);
    for (auto *action : scientificWindow.findChildren<QAction *>())
        if (action->text() == "Undo") action->trigger();
    assert(blocks->item(0)->text() == page.words[0].originalText);

    // The default page selection mixes a display equation, prose and inline math.
    {
        yagf::core::OcrPage mixed;
        mixed.width = mixed.sourceWidth = image.width();
        mixed.height = mixed.sourceHeight = image.height();
        mixed.words = {{1, 1, 1, 1, {10,10,90,18}, "x=1", "x=1", 95},
                       {1, 2, 1, 1, {5,45,40,20}, "Пусть", "Пусть", 90},
                       {1, 2, 1, 1, {50,45,45,20}, R"(\(x=1\))", R"(\(x=1\))", 90},
                       {1, 2, 1, 1, {105,45,75,20}, "верно.", "верно.", 90}};
        mixed.words[0].layoutBlock = mixed.words[2].layoutBlock = true;
        mixed.rebuildHierarchy();
        mixed.regions[0].type = "equation";
        yagf::core::OcrDocument mixedBook;
        mixedBook.sourcePath = sourcePath;
        mixedBook.replacePage(mixed);
        const QString mixedPath = directory.filePath("mixed-edit.yagf.json");
        assert(yagf::core::ProjectFile::save(mixedPath, mixedBook, &error));
        yagf::ui::MainWindow mixedWindow;
        mixedWindow.loadProject(mixedPath);
        auto *text = mixedWindow.findChild<QTextEdit *>("recognizedTextEditor");
        auto *apply = mixedWindow.findChild<QPushButton *>("applyButton");
        auto *list = mixedWindow.findChild<QListWidget *>("recognizedWords");
        assert(text && apply && list && !text->isReadOnly() && apply->isEnabled());
        text->setPlainText(QStringLiteral("x = 2\n\nПусть теперь \\(x = 2\\) истинно."));
        apply->click();
        assert(list->count() == 5 && list->item(0)->text() == "x = 2");
        assert(list->item(3)->text() == R"(\(x = 2\))" && list->item(4)->text() == "истинно.");
        list->setCurrentRow(3, QItemSelectionModel::ClearAndSelect);
        auto *edit = mixedWindow.findChild<QAction *>("editFormulaAction");
        assert(edit && edit->isEnabled());
        // Use real preview tools when installed, outside this test's fake OCR tool directory.
        const auto previousPath = qgetenv("PATH");
        qputenv("PATH", previousPath.mid(QFile::encodeName(directory.path()).size() + 1));
        const bool hasPreview = !QStandardPaths::findExecutable("pdflatex").isEmpty() &&
                                !QStandardPaths::findExecutable("pdftoppm").isEmpty();
        for (bool save : {false, true}) {
            QTimer::singleShot(0, &mixedWindow, [save, hasPreview] {
                auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                assert(dialog && dialog->objectName() == "formulaEditor");
                auto *input = dialog->findChild<QPlainTextEdit *>("formulaSource");
                auto *preview = dialog->findChild<QLabel *>("formulaPreview");
                assert(input && preview && input->toPlainText() == "x = 2");
                if (hasPreview && !save) {
                    input->setPlainText(R"(\unknowncommand{x})");
                    QElapsedTimer wait;
                    wait.start();
                    while (!preview->text().contains("Unsupported") && wait.elapsed() < 5000) QTest::qWait(20);
                    assert(preview->text().contains("unknowncommand"));
                }
                input->setPlainText(R"(\frac{x}{2})");
                if (hasPreview) {
                    QElapsedTimer wait;
                    wait.start();
                    while (preview->pixmap().isNull() && wait.elapsed() < 10000) QTest::qWait(20);
                    const auto rendered = preview->pixmap().toImage().convertToFormat(QImage::Format_Grayscale8);
                    assert(!rendered.isNull());
                    int ink = 0;
                    for (int y = 0; y < rendered.height(); ++y)
                        for (int x = 0; x < rendered.width(); ++x) ink += rendered.constScanLine(y)[x] < 150;
                    assert(ink > 10);
                }
                if (save) dialog->accept(); else dialog->reject();
            });
            edit->trigger();
            assert(list->item(3)->text() == (save ? R"(\(\frac{x}{2}\))" : R"(\(x = 2\))"));
        }
        qputenv("PATH", previousPath);
        for (auto *action : mixedWindow.findChildren<QAction *>())
            if (action->text() == "Save project") action->trigger();
        assert(yagf::core::ProjectFile::load(mixedPath, &mixedBook, &error));
        const auto &saved = mixedBook.pages.first();
        assert(saved.regions.first().type == "equation" && saved.words[3].layoutBlock);
        assert(saved.words[3].bounds == mixed.words[2].bounds && saved.words[3].id == mixed.words[2].id);
        assert(saved.words[3].text == R"(\(\frac{x}{2}\))" && !saved.words[1].reviewed);
        for (auto *action : mixedWindow.findChildren<QAction *>())
            if (action->text() == "Undo") action->trigger();
        assert(list->item(3)->text() == R"(\(x = 2\))");
        list->setCurrentRow(0, QItemSelectionModel::ClearAndSelect);
        qputenv("PATH", QFile::encodeName(noTools.path()));
        QTimer::singleShot(0, &mixedWindow, [] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            auto *input = dialog->findChild<QPlainTextEdit *>("formulaSource");
            assert(input->toPlainText() == "x = 2");
            assert(dialog->findChild<QLabel *>("formulaPreview")->text().contains("pdflatex", Qt::CaseInsensitive));
            input->setPlainText("x = 3");
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
        });
        edit->trigger();
        qputenv("PATH", previousPath);
        assert(list->item(0)->text() == "x = 3");
    }

    assert(!scientificWindow.usePaddleVlService(QUrl("file:///tmp/model")));
    assert(!scientificWindow.usePaddleVlService(QUrl("http://localhost:8765/v1/recognize")));
    assert(scientificWindow.usePaddleVlService(QUrl("http://127.0.0.1:8765")));
    auto *vlLanguage = scientificWindow.findChild<QLineEdit *>("languageField");
    assert(vlLanguage && vlLanguage->text() == "auto");
    bool vlStatus = false;
    for (auto *label : scientificWindow.findChildren<QLabel *>())
        vlStatus |= label->text().contains("OCR engine: PaddleOCR-VL");
    assert(vlStatus);

    // Opening DjVu enables direct PDF conversion before OCR and multipage navigation.
    const QString ppmPath = directory.filePath("djvu-page.ppm");
    assert(image.save(ppmPath, "PPM"));
    qputenv("YAGF_TEST_PPM", QFile::encodeName(ppmPath));
    const auto helperScript = [&](const QString &name, const QByteArray &body) {
        QFile helper(directory.filePath(name));
        assert(helper.open(QIODevice::WriteOnly));
        assert(helper.write(body) == body.size());
        helper.close();
        assert(helper.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    };
    helperScript("djvused", "#!/bin/sh\necho 2\n");
    helperScript("ddjvu", "#!/bin/sh\nfor arg do last=$arg; done\ncp \"$YAGF_TEST_PPM\" \"$last\"\n");
    const QString djvuPath = directory.filePath("source book.DJV");
    QFile djvu(djvuPath);
    assert(djvu.open(QIODevice::WriteOnly));
    djvu.write("DjVu fixture handled by test helpers");
    djvu.close();
    yagf::ui::MainWindow djvuWindow;
    djvuWindow.loadSource(djvuPath);
    auto *convert = djvuWindow.findChild<QAction *>("convertDjVuAction");
    auto *pages = djvuWindow.findChild<QListWidget *>("pageMiniatures");
    assert(convert && pages);
    timer.restart();
    while (!convert->isEnabled() && timer.elapsed() < 5000) {
        QApplication::processEvents();
        QThread::msleep(5);
    }
    assert(convert->isEnabled() && pages->count() == 2);
    auto *djvuWords = djvuWindow.findChild<QListWidget *>("recognizedWords");
    assert(djvuWords->count() == 0);
    const QString converted = directory.filePath("converted-before-ocr.pdf");
    QTimer::singleShot(0, &djvuWindow, [&] {
        auto *dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
        assert(dialog && dialog->windowTitle() == "Convert DjVu to PDF");
        dialog->selectFile(converted);
        QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection);
    });
    convert->trigger();
    timer.restart();
    while ((!QFileInfo::exists(converted) || !convert->isEnabled()) && timer.elapsed() < 5000) {
        QApplication::processEvents();
        QThread::msleep(5);
    }
    QFile convertedPdf(converted);
    assert(convertedPdf.open(QIODevice::ReadOnly) && convertedPdf.read(5) == "%PDF-");
    assert(djvuWords->count() == 0);

    // Release 2.1: queue navigation renders the target page, verifies with undo,
    // and DPI changes preserve corrected text, boxes, and saved project metadata.
    const QString resolutionPdf = directory.filePath("resolution-source.pdf");
    {
        QPdfWriter writer(resolutionPdf);
        writer.setResolution(150);
        writer.setPageSize(QPageSize(QSizeF(288, 384), QPageSize::Point));
        writer.setPageMargins(QMarginsF(0,0,0,0));
        QPainter painter(&writer);
        painter.fillRect(0,0,600,800,Qt::white); painter.drawText(20,40,"Page one");
        assert(writer.newPage()); painter.fillRect(0,0,600,800,Qt::white); painter.drawText(20,40,"Page two");
    }
    yagf::core::OcrDocument resolutionBook;
    resolutionBook.engine = "tesseract"; resolutionBook.language = "en";
    resolutionBook.sourcePath = resolutionPdf; resolutionBook.totalPages = 2;
    yagf::core::OcrPage resolutionPage;
    resolutionPage.number = 1; resolutionPage.engineId = "tesseract"; resolutionPage.language = "en";
    resolutionPage.width = resolutionPage.sourceWidth = 600;
    resolutionPage.height = resolutionPage.sourceHeight = 800;
    resolutionPage.words.append({1,1,1,1,QRect(20,30,80,20),"modem","modern",95,true});
    resolutionBook.replacePage(resolutionPage);
    resolutionPage = yagf::core::rescalePageGeometry(resolutionPage,1200,1600,1200,1600,300);
    resolutionPage.number = 2; resolutionPage.words[0].page = 2;
    resolutionPage.words[0].id = "page-two-word"; resolutionPage.words[0].reviewed = false; resolutionPage.words[0].confidence = 30;
    resolutionBook.replacePage(resolutionPage);
    const QString resolutionProject = directory.filePath("resolution.yagf.json");
    assert(yagf::core::ProjectFile::save(resolutionProject, resolutionBook, &error));
    yagf::ui::MainWindow resolutionWindow;
    resolutionWindow.show(); resolutionWindow.loadProject(resolutionProject);
    auto *projectProgress = resolutionWindow.findChild<QProgressDialog *>("projectLoadingProgress");
    assert(projectProgress && projectProgress->isVisible() && projectProgress->minimum() == 0 && projectProgress->maximum() == 0);
    assert(!resolutionWindow.findChild<QAction *>("cancelRecognitionAction")->isVisible());
    auto *inheritedLanguage = resolutionWindow.findChild<QLineEdit *>("languageField");
    assert(inheritedLanguage && inheritedLanguage->text() == "eng");
    auto *resolutionView = resolutionWindow.findChild<yagf::ui::PageView *>();
    auto *resolutionQueue = resolutionWindow.findChild<yagf::ui::VerificationController *>();
    auto waitUi = [&](auto condition) {
        timer.restart();
        while (!condition() && timer.elapsed() < 10000) { QApplication::processEvents(); QThread::msleep(5); }
        assert(condition());
    };
    waitUi([&] { return resolutionView->scene()->sceneRect().width() == 600; });
    assert(!projectProgress->isVisible());
    resolutionWindow.findChild<QAction *>("showVerificationQueue")->trigger();
    auto *issues = resolutionWindow.findChild<QListView *>("verificationItems");
    assert(issues && issues->model()->rowCount() == 1);
    resolutionQueue->navigate(1);
    waitUi([&] { return resolutionView->scene()->sceneRect().width() == 1200; });
    assert(inheritedLanguage->text() == "eng");
    auto *pageExport = resolutionWindow.findChild<QAction *>("exportCurrentPageAction");
    assert(pageExport && pageExport->isEnabled());
    const auto pageStem = directory.filePath("exported-page");
    QTimer::singleShot(0, &resolutionWindow, [&] {
        auto *dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
        assert(dialog && dialog->selectedNameFilter().contains("*.docx"));
        assert(QFileInfo(dialog->selectedFiles().first()).baseName().endsWith("-page-2"));
        dialog->selectNameFilter("Layout HTML (*.html)");
        dialog->selectFile(pageStem);
        QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection);
    });
    pageExport->trigger();
    QFile singlePage(pageStem + ".html");
    waitUi([&] { return pageExport->isEnabled() && singlePage.exists(); });
    assert(singlePage.open(QIODevice::ReadOnly));
    const auto pageHtml = singlePage.readAll();
    assert(pageHtml.contains("Page 2") && !pageHtml.contains("Page 1") && pageHtml.contains("modern"));
    if (!QStandardPaths::findExecutable("pandoc").isEmpty() && !QStandardPaths::findExecutable("unzip").isEmpty()) {
        const auto pageDocx = pageStem + ".docx";
        QTimer::singleShot(0, &resolutionWindow, [&] {
            auto *dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
            assert(dialog && dialog->selectedNameFilter().contains("*.docx"));
            dialog->selectFile(pageDocx);
            QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection);
        });
        pageExport->trigger();
        waitUi([&] { return pageExport->isEnabled() && QFileInfo::exists(pageDocx); });
        QProcess unzip;
        unzip.start("unzip", {"-p", pageDocx, "word/document.xml"});
        assert(unzip.waitForFinished() && unzip.exitCode() == 0);
        const auto xml = unzip.readAllStandardOutput();
        assert(xml.count("modern") == 1 && !xml.contains("w:type=\"page\""));
    }
    auto *resolutionWords = resolutionWindow.findChild<QListWidget *>("recognizedWords");
    assert(resolutionWords->selectedItems().size() == 1);
    for (auto *button : resolutionWindow.findChildren<QPushButton *>())
        if (button->text() == "Mark verified") button->click();
    waitUi([&] { return issues->model()->rowCount() == 0; });
    auto *rendering = resolutionWindow.findChild<QAction *>("renderingResolution");
    assert(rendering && rendering->isEnabled());
    QTimer::singleShot(0, &resolutionWindow, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        assert(dialog);
        auto *dpi = dialog->findChild<QComboBox *>("renderDpi");
        auto *current = dialog->findChild<QCheckBox *>("applyRenderDpiToCurrent");
        assert(dpi && current); dpi->setCurrentIndex(dpi->findData(200)); current->setChecked(true); dialog->accept();
    });
    rendering->trigger();
    waitUi([&] { return resolutionView->scene()->sceneRect().width() == 800; });
    auto saveResolution = [&] {
        for (auto *action : resolutionWindow.findChildren<QAction *>())
            if (action->text() == QStringLiteral("Save project")) { action->trigger(); return; }
        assert(false);
    };
    saveResolution();
    yagf::core::OcrDocument resolutionLoaded;
    assert(yagf::core::ProjectFile::load(resolutionProject, &resolutionLoaded, &error));
    assert(resolutionLoaded.rendering.dpi == 200 && resolutionLoaded.page(2)->renderDpi == 200);
    assert(resolutionLoaded.page(1)->renderDpi == 150 && resolutionLoaded.page(2)->words[0].text == "modern");
    // Pixel-edge mappings round outward so no source ink is cropped away.
    assert(resolutionLoaded.page(2)->words[0].reviewed && resolutionLoaded.page(2)->words[0].bounds.x() == 26);
    for (auto *action : resolutionWindow.findChildren<QAction *>())
        if (action->shortcut() == QKeySequence::Undo) action->trigger();
    waitUi([&] { return resolutionView->scene()->sceneRect().width() == 1200; });
    saveResolution();
    assert(yagf::core::ProjectFile::load(resolutionProject, &resolutionLoaded, &error));
    assert(resolutionLoaded.page(2)->renderDpi == 300 && !resolutionLoaded.page(2)->words[0].reviewed);

    {
        auto book=resolutionBook;
        book.pages[0].imageRegions={{"empty-math",{12,90,120,40},{},false,"math"}};
        auto &word=book.pages[1].words[0];
        word.text=word.originalText="Compare $x$ with $y+1$.";
        const auto project=directory.filePath("navigator.yagf.json");
        assert(yagf::core::ProjectFile::save(project,book,&error));
        yagf::ui::MainWindow w; w.show(); w.loadProject(project);
        auto *view=w.findChild<yagf::ui::PageView *>();
        waitUi([&]{return view->scene()->sceneRect().width()==600;});
        auto *navigator=w.findChild<yagf::ui::VerificationController *>();
        navigator->show();
        w.findChild<QPushButton *>("navigatorCategory1")->click();
        navigator->navigate(1);
        auto *blocks=w.findChild<QListWidget *>("imageRegions");
        assert(blocks->selectedItems().size()==1 && blocks->selectedItems()[0]->data(Qt::UserRole)=="empty-math");
        navigator->navigate(1);
        auto *editor=w.findChild<QTextEdit *>("recognizedTextEditor");
        waitUi([&]{return view->scene()->sceneRect().width()==1200 && editor->textCursor().selectedText()=="x";});
        navigator->navigate(1);
        assert(editor->textCursor().selectedText()=="y+1");
        bool opened=false;
        QTimer::singleShot(0,&w,[&]{
            auto *dialog=qobject_cast<QDialog *>(QApplication::activeModalWidget());
            assert(dialog && dialog->objectName()=="formulaEditor");
            auto *source=dialog->findChild<QPlainTextEdit *>("formulaSource");
            assert(source && source->toPlainText()=="y+1");
            opened=true; dialog->reject();
        });
        for (auto *button:w.findChildren<QPushButton *>())
            if (button->text()=="Edit formula…") button->click();
        assert(opened);
        w.findChild<QPushButton *>("navigatorCategory2")->click();
        navigator->navigate(1);
        assert(editor->toPlainText()==word.text);
        navigator->navigate(1);
        waitUi([&]{return view->scene()->sceneRect().width()==600 && blocks->selectedItems().size()==1;});
        assert(blocks->selectedItems()[0]->data(Qt::UserRole)=="empty-math");
    }

    auto retained = resolutionLoaded.page(2)->words[0];
    retained.id = "retained-edit"; retained.text = "restored"; retained.reviewed = true;
    resolutionLoaded.page(2)->pendingCorrections = {retained};
    const QString pendingProject = directory.filePath("pending.yagf.json");
    assert(yagf::core::ProjectFile::save(pendingProject, resolutionLoaded, &error));
    yagf::ui::MainWindow pendingWindow;
    pendingWindow.show(); pendingWindow.loadProject(pendingProject);
    auto *pendingView = pendingWindow.findChild<yagf::ui::PageView *>();
    waitUi([&] { return pendingView->scene()->sceneRect().width() == 600; });
    auto *pendingQueue = pendingWindow.findChild<yagf::ui::VerificationController *>();
    pendingQueue->show();
    auto *pendingFilter = pendingWindow.findChild<QComboBox *>("verificationFilter");
    pendingFilter->setCurrentIndex(pendingFilter->findData(yagf::core::PendingCorrection));
    auto *pendingItems = pendingWindow.findChild<QListView *>("verificationItems");
    assert(pendingItems->model()->rowCount() == 1);
    pendingQueue->navigate(1);
    waitUi([&] { return pendingView->scene()->sceneRect().width() == 1200; });
    QTimer::singleShot(0, &pendingWindow, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        assert(dialog && dialog->objectName() == "aiReviewDialog");
        auto *reading = dialog->findChild<QPlainTextEdit *>("verifiedPassage");
        assert(reading && reading->toPlainText() == "restored"); dialog->accept();
    });
    for (auto *button : pendingWindow.findChildren<QPushButton *>())
        if (button->text() == "Review / restore…") button->click();
    waitUi([&] { return pendingItems->model()->rowCount() == 0; });
    for (auto *action : pendingWindow.findChildren<QAction *>())
        if (action->text() == "Save project") action->trigger();
    yagf::core::OcrDocument pendingLoaded;
    assert(yagf::core::ProjectFile::load(pendingProject, &pendingLoaded, &error));
    assert(pendingLoaded.page(2)->pendingCorrections.isEmpty() && pendingLoaded.page(2)->words[0].text == "restored");
    for (auto *action : pendingWindow.findChildren<QAction *>())
        if (action->shortcut() == QKeySequence::Undo) action->trigger();
    waitUi([&] { return pendingItems->model()->rowCount() == 1; });

    // Changing only the default before batch OCR refreshes the current preview
    // when the batch creates that page at a different DPI.
    // This fixture supplies only full-page readings, with no extra crop readings.
    helperScript("tesseract", QByteArray("#!/bin/sh\n"
        "case \" $* \" in *' --psm 7 '*) head -n 1 \"$YAGF_TEST_TSV\"; exit 0;; esac\n")
        + body.mid(body.indexOf('\n') + 1));
    yagf::core::OcrDocument emptyBook;
    emptyBook.sourcePath = resolutionPdf; emptyBook.totalPages = 2;
    const QString batchResolutionProject = directory.filePath("batch-resolution.yagf.json");
    assert(yagf::core::ProjectFile::save(batchResolutionProject, emptyBook, &error));
    yagf::ui::MainWindow batchResolutionWindow;
    batchResolutionWindow.show(); batchResolutionWindow.loadProject(batchResolutionProject);
    auto *batchView = batchResolutionWindow.findChild<yagf::ui::PageView *>();
    waitUi([&] { return batchView->scene()->sceneRect().width() == 600; });
    QTimer::singleShot(0, &batchResolutionWindow, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()); assert(dialog);
        auto *dpi = dialog->findChild<QComboBox *>("renderDpi"); dpi->setCurrentIndex(dpi->findData(200)); dialog->accept();
    });
    batchResolutionWindow.findChild<QAction *>("renderingResolution")->trigger();
    auto *recognizeAll = batchResolutionWindow.findChild<QAction *>("recognizeAllAction");
    assert(recognizeAll && recognizeAll->isEnabled() && recognizeAll->iconText() == "Recognize all");
    recognizeAll->trigger();
    assert(!recognizeAll->isEnabled());
    auto *batchProgress = batchResolutionWindow.findChild<QProgressDialog *>("recognitionProgressDialog");
    assert(batchProgress && batchProgress->isVisible());
    assert(batchProgress->value() == 0 && batchProgress->maximum() == 2);
    assert(batchProgress->labelText() == "0 / 2 pages recognized");
    QSignalSpy progressValues(batchProgress->findChild<QProgressBar *>(), &QProgressBar::valueChanged);
    bool sawFirstPage = false;
    QObject::connect(batchProgress->findChild<QProgressBar *>(), &QProgressBar::valueChanged,
                     &batchResolutionWindow, [&](int value) {
        if (value == 1) {
            assert(batchProgress->isVisible());
            assert(batchProgress->labelText() == "1 / 2 pages recognized");
            sawFirstPage = true;
        }
    });
    waitUi([&] { return batchView->scene()->sceneRect().width() == 800; });
    auto *batchWords = batchResolutionWindow.findChild<QListWidget *>("recognizedWords");
    waitUi([&] { return recognizeAll->isEnabled(); });
    assert(batchWords->count() == 2);
    assert(sawFirstPage && progressValues.contains(QList<QVariant>{2}));
    assert(!batchProgress->isVisible());
    for (auto *action : batchResolutionWindow.findChildren<QAction *>())
        if (action->text() == "Save project") action->trigger();
    yagf::core::OcrDocument batchLoaded;
    assert(yagf::core::ProjectFile::load(batchResolutionProject, &batchLoaded, &error));
    assert(batchLoaded.pages.size() == 2 && batchLoaded.page(1)->renderDpi == 200 && batchLoaded.page(2)->renderDpi == 200);

    // The multipage dialog interrupts the same queue as the toolbar and can be reused.
    yagf::ui::MainWindow cancelBatchWindow;
    cancelBatchWindow.show(); cancelBatchWindow.loadSource(resolutionPdf);
    auto *cancelRecognizeAll = cancelBatchWindow.findChild<QAction *>("recognizeAllAction");
    waitUi([&] { return cancelRecognizeAll->isEnabled(); });
    cancelRecognizeAll->trigger();
    auto *cancelProgress = cancelBatchWindow.findChild<QProgressDialog *>("recognitionProgressDialog");
    assert(cancelProgress && cancelProgress->isVisible());
    auto *cancelProgressButton = cancelProgress->findChild<QPushButton *>();
    assert(cancelProgressButton && cancelProgressButton->text() == "Interrupt");
    cancelProgressButton->click();
    waitUi([&] { return cancelRecognizeAll->isEnabled(); });
    assert(!cancelProgress->isVisible());
    cancelRecognizeAll->trigger();
    assert(cancelProgress->isVisible() && cancelProgress->value() == 0 && !cancelProgress->wasCanceled());
    waitUi([&] { return cancelRecognizeAll->isEnabled(); });
    assert(!cancelProgress->isVisible());

    // Page menus clear the clicked page's blocks, preserve text, and support undo.
    {
        auto clearBook = resolutionBook;
        for (auto &page : clearBook.pages)
            page.imageRegions = {{"first", {10, 10, 100, 100}}, {"second", {130, 10, 100, 100}, "rus", false, "text"}};
        const QString clearProject = directory.filePath("clear-blocks.yagf.json");
        assert(yagf::core::ProjectFile::save(clearProject, clearBook, &error));
        yagf::ui::MainWindow clearWindow;
        clearWindow.show(); clearWindow.loadProject(clearProject);
        auto *clearPages = clearWindow.findChild<QListWidget *>("pageMiniatures");
        auto *clearRegions = clearWindow.findChild<QListWidget *>("imageRegions");
        auto *clearNumber = clearWindow.findChild<QSpinBox *>("pageNumber");
        waitUi([&] { return clearRegions->count() == 2 && clearPages->count() == 2; });
        // Page keys navigate the document even in widgets that normally scroll.
        auto *pageView = clearWindow.findChild<yagf::ui::PageView *>();
        auto *pageWords = clearWindow.findChild<QListWidget *>("recognizedWords");
        auto *pageEditor = clearWindow.findChild<QTextEdit *>("recognizedTextEditor");
        for (auto *target : QVector<QWidget *>{pageView, pageWords, pageEditor, clearPages, clearNumber}) {
            target->setFocus();
            QTest::keyClick(target, Qt::Key_PageDown);
            waitUi([&] { return clearNumber->value() == 2 && clearNumber->isEnabled(); });
            QTest::keyClick(target, Qt::Key_PageDown);
            assert(clearNumber->value() == 2);
            QTest::keyClick(target, Qt::Key_PageUp);
            waitUi([&] { return clearNumber->value() == 1 && clearNumber->isEnabled(); });
            QTest::keyClick(target, Qt::Key_PageUp);
            assert(clearNumber->value() == 1);
        }
        QTest::keyClick(pageEditor, Qt::Key_PageDown, Qt::ShiftModifier);
        assert(clearNumber->value() == 1);
        {
            QDialog dialog(&clearWindow);
            auto *input = new QLineEdit(&dialog);
            dialog.show();
            QTest::keyClick(input, Qt::Key_PageDown);
            assert(clearNumber->value() == 1);
        }
        const auto pageMenu = [&](int number, bool enabled, bool preview = false) {
            clearPages->scrollToItem(clearPages->item(number-1));
            clearPages->doItemsLayout();
            const QPoint position = clearPages->visualItemRect(clearPages->item(number-1)).center();
            QTimer::singleShot(0, &clearWindow, [&, enabled] {
                auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
                assert(menu && menu->objectName() == "pageContextMenu");
                auto *action = menu->findChild<QAction *>("clearPageBlocks");
                assert(action && action->text() == "Clear all blocks" && action->isEnabled() == enabled);
                if (enabled) action->trigger();
                menu->close();
            });
            if (preview) {
                auto *view = clearWindow.findChild<yagf::ui::PageView *>();
                const QPoint point = view->mapFromScene(QPointF(400, 400));
                QContextMenuEvent event(QContextMenuEvent::Mouse, point, view->viewport()->mapToGlobal(point));
                QApplication::sendEvent(view->viewport(), &event);
            } else {
                QContextMenuEvent event(QContextMenuEvent::Mouse, position, clearPages->viewport()->mapToGlobal(position));
                QApplication::sendEvent(clearPages->viewport(), &event);
            }
        };
        const auto saveClearProject = [&] {
            for (auto *action : clearWindow.findChildren<QAction *>())
                if (action->text() == "Save project") action->trigger();
            yagf::core::OcrDocument saved;
            assert(yagf::core::ProjectFile::load(clearProject, &saved, &error));
            assert(saved.plainText() == clearBook.plainText());
            return saved;
        };
        QAction *clearUndo = nullptr, *clearRedo = nullptr;
        for (auto *action : clearWindow.findChildren<QAction *>()) {
            if (action->shortcut() == QKeySequence::Undo) clearUndo = action;
            if (action->shortcut() == QKeySequence::Redo) clearRedo = action;
        }
        assert(clearUndo && clearRedo);
        pageMenu(2, true);
        assert(clearNumber->value() == 1 && clearRegions->count() == 2);
        auto saved = saveClearProject();
        assert(saved.page(1)->imageRegions.size() == 2 && saved.page(2)->imageRegions.isEmpty());
        assert(saved.page(1)->layoutTraining.examples.isEmpty());
        const auto cleared = saved.page(2)->layoutTraining.examples;
        assert(cleared.size() == 1 && cleared[0].proposed.size() == 2 && cleared[0].corrected.isEmpty());
        assert(QImage::fromData(cleared[0].png).size() == QSize(saved.page(2)->width, saved.page(2)->height));
        pageMenu(2, false);
        clearUndo->trigger();
        saved = saveClearProject();
        assert(saved.page(2)->imageRegions.size() == 2 && saved.page(2)->layoutTraining.examples.isEmpty());
        clearRedo->trigger();
        assert(saveClearProject().page(2)->imageRegions.isEmpty());
        pageMenu(1, true, true);
        assert(clearRegions->count() == 0);
        pageMenu(1, false, true);
        clearUndo->trigger();
        assert(clearRegions->count() == 2);
    }

    // Correct skew adds the residual tilt and maps OCR/corrections through the
    // original coordinate frame. Undo restores the prior image and geometry.
    {
        using yagf::processing::ImageProcessor;
        QImage clean(900, 650, QImage::Format_ARGB32);
        clean.fill(Qt::white);
        {
            QPainter painter(&clean);
            painter.setPen(Qt::NoPen);
            painter.setBrush(Qt::black);
            for (int line = 0; line < 7; ++line)
                for (int word = 0; word < 6; ++word)
                    painter.drawRect(90+word*115, 80+line*75, 55+(word%3)*11, 10);
        }
        const QImage tilted = clean.transformed(QTransform().rotate(4), Qt::SmoothTransformation);
        const QString skewSource = directory.filePath("skew.png");
        assert(tilted.save(skewSource));
        yagf::core::OcrDocument skewBook;
        skewBook.sourcePath = skewSource;
        skewBook.engine = "tesseract"; skewBook.language = "eng"; skewBook.totalPages = 1;
        yagf::core::OcrPage skewPage;
        skewPage.number = 1; skewPage.engineId = "tesseract"; skewPage.language = "eng";
        skewPage.sourceWidth = tilted.width(); skewPage.sourceHeight = tilted.height();
        skewPage.preprocessing = {.deskewDegrees = 2, .autoContrast = true};
        const auto partiallyCorrected = ImageProcessor::apply(tilted, skewPage.preprocessing);
        assert(partiallyCorrected);
        skewPage.width = partiallyCorrected->width(); skewPage.height = partiallyCorrected->height();
        skewPage.words.append({1,1,1,1,QRect(100,100,50,12),"text","text",95,true});
        skewPage.imageRegions = {{"skew-block", {100,100,100,80}, "eng", false, "text"}};
        yagf::core::OcrReading companion;companion.source="skew-fixture";companion.engine="doctr-remote";companion.label="Other OCR";companion.language="eng";companion.area={0,0,skewPage.width,skewPage.height};companion.items.append({{100,100,50,12},"text",1,1,1,false});skewPage.ocrReadings.append(companion);
        skewBook.replacePage(skewPage);
        const QString skewProject = directory.filePath("skew.yagf.json");
        assert(yagf::core::ProjectFile::save(skewProject, skewBook, &error));
        yagf::ui::MainWindow skewWindow;
        skewWindow.show(); skewWindow.loadProject(skewProject);
        auto *skewAction = skewWindow.findChild<QAction *>("correctSkewAction");
        auto *undoSkew = skewWindow.findChild<QAction *>("undoSkewCorrectionAction");
        auto *skewView = skewWindow.findChild<yagf::ui::PageView *>();
        auto *skewWords = skewWindow.findChild<QListWidget *>("recognizedWords");
        auto *skewBlocks = skewWindow.findChild<QListWidget *>("imageRegions");
        assert(skewAction && undoSkew && skewView && skewWords && skewBlocks);
        waitUi([&] { return skewAction->isEnabled() && skewWords->count() == 1; });
        assert(!undoSkew->isVisible() && !undoSkew->isEnabled());
        const auto displayedImage = [&] {
            for (auto *item : skewView->scene()->items())
                if (auto *pixmap = qgraphicsitem_cast<QGraphicsPixmapItem *>(item))
                    return pixmap->pixmap().toImage();
            return QImage{};
        };
        const QImage before = displayedImage();
        const auto beforeAssessment = ImageProcessor::assess(before);
        assert(beforeAssessment && std::abs(beforeAssessment->skewDegrees-2) < .6);
        skewAction->trigger();
        assert(skewWords->count() == 1 && skewBlocks->count() == 1);
        assert(undoSkew->isVisible() && undoSkew->isEnabled() && !undoSkew->icon().isNull());
        const auto afterAssessment = ImageProcessor::assess(displayedImage());
        assert(afterAssessment && std::abs(afterAssessment->skewDegrees) < 1);
        for (auto *action : skewWindow.findChildren<QAction *>())
            if (action->text() == "Save project") action->trigger();
        yagf::core::OcrDocument saved;
        assert(yagf::core::ProjectFile::load(skewProject, &saved, &error));
        assert(std::abs(saved.page(1)->preprocessing.deskewDegrees-4) < .6);
        assert(saved.page(1)->preprocessing.autoContrast);
        assert(saved.page(1)->ocrReadings.size()==1);
        assert(saved.page(1)->words[0].text=="text" && saved.page(1)->words[0].reviewed);
        assert(saved.page(1)->words[0].id==skewBook.page(1)->words[0].id);
        assert(saved.page(1)->words[0].bounds!=skewBook.page(1)->words[0].bounds);
        assert(saved.page(1)->coordinates.isDefined() && !saved.page(1)->sourceGeometry.isEmpty());
        QAction *skewUndo = nullptr, *skewRedo = nullptr;
        for (auto *action : skewWindow.findChildren<QAction *>()) {
            if (action->shortcut() == QKeySequence::Undo) skewUndo = action;
            if (action->shortcut() == QKeySequence::Redo) skewRedo = action;
        }
        assert(skewUndo && skewRedo);
        // A later rotation has its own undo and must not be undone by this button.
        skewWindow.findChild<QAction *>("rotateImageRightAction")->trigger();
        const QImage rotated = displayedImage();
        assert(!undoSkew->isVisible() && !undoSkew->isEnabled());
        undoSkew->trigger();
        assert(displayedImage() == rotated);
        skewUndo->trigger();
        assert(undoSkew->isVisible() && undoSkew->isEnabled());
        undoSkew->trigger();
        assert(skewWords->count() == 1 && skewBlocks->count() == 1 && displayedImage() == before);
        assert(skewWindow.findChild<QAction *>("ocrReadingsAction")->isEnabled());
        assert(!undoSkew->isVisible() && !undoSkew->isEnabled());
        skewRedo->trigger();
        assert(skewWords->count() == 1 && skewBlocks->count()==1 && undoSkew->isVisible() && undoSkew->isEnabled());
        skewUndo->trigger();
        assert(skewWords->count() == 1 && !undoSkew->isVisible());
    }

    // A selected character opens the teaching workflow with its source image;
    // saving and undoing a book-specific draft use the normal project history.
    {
        const auto pixels=symbolFixture();
        yagf::core::OcrDocument symbolBook;
        symbolBook.sourcePath=directory.filePath("teach-symbol.png"); assert(pixels.save(symbolBook.sourcePath));
        symbolBook.engine="tesseract"; symbolBook.language="rus";
        yagf::core::OcrPage page; page.number=1; page.engineId="tesseract"; page.language="rus";
        page.width=page.sourceWidth=pixels.width(); page.height=page.sourceHeight=pixels.height();
        page.words.append({1,1,1,1,pixels.rect(),"ф","ф",45}); symbolBook.replacePage(page);
        const auto project=directory.filePath("teach-symbol.yagf.json"); assert(yagf::core::ProjectFile::save(project,symbolBook,&error));
        yagf::ui::MainWindow symbolWindow; symbolWindow.show(); symbolWindow.loadProject(project);
        auto *words=symbolWindow.findChild<QListWidget *>("recognizedWords");
        waitUi([&] { return words->count()==1; }); words->setCurrentRow(0);
        auto *editor=symbolWindow.findChild<QTextEdit *>("recognizedTextEditor");
        auto cursor=editor->textCursor(); cursor.setPosition(0); cursor.setPosition(1,QTextCursor::KeepAnchor); editor->setTextCursor(cursor);
        auto *teach=symbolWindow.findChild<QAction *>("teachSelectedSymbolAction"); assert(teach && teach->isEnabled());
        bool taught=false;
        QTimer teacher;
        QObject::connect(&teacher,&QTimer::timeout,&symbolWindow,[&] {
            auto *dialog=dynamic_cast<yagf::ui::SymbolRulesDialog *>(QApplication::activeModalWidget());
            if (!dialog) return;
            auto *save=dialog->findChild<QPushButton *>("saveSymbolCrop"); if (!save->isEnabled()) return;
            teacher.stop();
            auto *label=dialog->findChild<QLineEdit *>("symbolRuleLabel"); assert(label->text()=="ф"); label->setText("ψ");
            save->click(); assert(dialog->bookRules().size()==1 && dialog->bookRules()[0].examples.size()==1);
            auto *buttons=dialog->findChild<QDialogButtonBox *>(); buttons->button(QDialogButtonBox::Save)->click(); taught=true;
        }); teacher.start(10); teach->trigger(); assert(taught);
        const auto saveBook=[&] {
            for (auto *action:symbolWindow.findChildren<QAction *>()) if (action->text()=="Save project") action->trigger();
            yagf::core::OcrDocument saved; assert(yagf::core::ProjectFile::load(project,&saved,&error)); return saved;
        };
        const auto saved=saveBook(); assert(saved.symbolRules.size()==1 && saved.symbolRules[0].label=="ψ");
        assert(!yagf::core::symbolRuleTrained(saved.symbolRules[0]) && saved.page(1)->words[0].text=="ф");
        for (auto *action:symbolWindow.findChildren<QAction *>()) if (action->shortcut()==QKeySequence::Undo) action->trigger();
        assert(saveBook().symbolRules.isEmpty());
    }

    // Unicode output is reversible, keeps formula editing and LaTeX export,
    // and block copying uses source pixels from the processed display.
    {
        QImage source(200,100,QImage::Format_RGB32);
        for (int y=0;y<source.height();++y) for (int x=0;x<source.width();++x) source.setPixel(x,y,qRgb(x,y,(x+y)%256));
        yagf::core::OcrDocument book; book.sourcePath=directory.filePath("formula-output.png"); assert(source.save(book.sourcePath));
        book.engine="fixture"; book.language="rus";
        yagf::core::OcrPage page; page.number=1; page.sourceWidth=200; page.sourceHeight=100;
        page.preprocessing={.deskewDegrees=2,.quarterTurns=1};
        const auto processed=yagf::processing::ImageProcessor::apply(source,page.preprocessing); assert(processed);
        page.width=processed->width(); page.height=processed->height(); page.language="rus"; page.engineId="fixture";
        page.words.append({1,1,1,1,{5,5,90,20},R"(справа: \(p_3=2\))",R"(справа: \(p_3=2\))",90}); page.words[0].layoutBlock=true;
        page.imageRegions={{"copy-block",{10,40,50,60},{},true}}; book.replacePage(page);
        const auto project=directory.filePath("formula-output.yagf.json"); assert(yagf::core::ProjectFile::save(project,book,&error));
        yagf::ui::MainWindow formulaWindow; formulaWindow.show(); formulaWindow.loadProject(project);
        auto *items=formulaWindow.findChild<QListWidget *>("recognizedWords"); waitUi([&] { return items->count()==1; });
        auto *unicode=formulaWindow.findChild<QAction *>("unicodeFormulaOutputAction"); assert(unicode && !unicode->isChecked()); unicode->setChecked(true);
        items->setCurrentRow(0);
        auto *editor=formulaWindow.findChild<QTextEdit *>("recognizedTextEditor"); assert(editor->toPlainText()=="справа: p₃=2");
        const auto saveBook=[&] {
            for (auto *action:formulaWindow.findChildren<QAction *>()) if (action->text()=="Save project") action->trigger();
            yagf::core::OcrDocument saved; assert(yagf::core::ProjectFile::load(project,&saved,&error)); return saved;
        };
        const auto saved=saveBook(); assert(saved.unicodeFormulas && saved.page(1)->words[0].latexText==R"(справа: \(p_3=2\))");
        QAction *undo=nullptr;
        for (auto *action:formulaWindow.findChildren<QAction *>()) if (action->shortcut()==QKeySequence::Undo) undo=action;
        assert(undo);
        QTimer::singleShot(0,&formulaWindow,[&] {
            auto *dialog=qobject_cast<QDialog *>(QApplication::activeModalWidget()); assert(dialog && dialog->objectName()=="formulaEditor");
            auto *text=dialog->findChild<QPlainTextEdit *>("formulaSource"); assert(text && text->toPlainText()=="p_3=2"); text->setPlainText("p_3=7");
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
        });
        auto *edit=formulaWindow.findChild<QAction *>("editFormulaAction"); assert(edit && edit->isEnabled()); edit->trigger();
        assert(editor->toPlainText()=="справа: p₃=7"); undo->trigger();
        items->setCurrentRow(0); assert(editor->toPlainText()=="справа: p₃=2");
        unicode->setChecked(false); assert(!unicode->isChecked()); items->setCurrentRow(0); assert(editor->toPlainText()==R"(справа: \(p_3=2\))");
        undo->trigger(); assert(unicode->isChecked());
        const auto tex=directory.filePath("ui-document.tex");
        QTimer::singleShot(0,&formulaWindow,[&] {
            auto *dialog=qobject_cast<QFileDialog *>(QApplication::activeModalWidget()); assert(dialog && dialog->windowTitle()=="Export LaTeX document");
            dialog->selectFile(tex); QMetaObject::invokeMethod(dialog,"accept",Qt::DirectConnection);
        });
        formulaWindow.findChild<QAction *>("exportLatexDocumentAction")->trigger();
        waitUi([&] { return QFile::exists(tex) && unicode->isEnabled(); });
        QFile output(tex); assert(output.open(QIODevice::ReadOnly)); const auto data=output.readAll(); assert(data.contains("p_3=2") && data.contains("\\begin{document}"));
        auto *view=formulaWindow.findChild<yagf::ui::PageView *>(); view->zoomBy(1.7); view->centerOn(page.imageRegions[0].bounds.center());
        QApplication::clipboard()->clear();
        const auto title=formulaWindow.windowTitle();
        QTimer::singleShot(0,&formulaWindow,[&] {
            auto *menu=qobject_cast<QMenu *>(QApplication::activePopupWidget()); assert(menu && menu->objectName()=="blockContextMenu");
            auto *copy=menu->findChild<QAction *>("copyBlockImageAction"); assert(copy && copy->isEnabled()); copy->trigger(); menu->close();
        });
        const auto point=view->mapFromScene(page.imageRegions[0].bounds.center());
        QContextMenuEvent event(QContextMenuEvent::Mouse,point,view->viewport()->mapToGlobal(point)); QApplication::sendEvent(view->viewport(),&event);
        const auto clipboard=QApplication::clipboard()->image().convertToFormat(QImage::Format_ARGB32);
        assert(clipboard==processed->copy(page.imageRegions[0].bounds).convertToFormat(QImage::Format_ARGB32));
        assert(formulaWindow.windowTitle()==title);
    }

    // AI classification is reviewed before applying and becomes one undoable page edit.
    {
        QTcpServer server; assert(server.listen(QHostAddress::LocalHost));
        QObject::connect(&server,&QTcpServer::newConnection,&server,[&] {
            auto *socket=server.nextPendingConnection();
            QObject::connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
            QObject::connect(socket,&QTcpSocket::readyRead,socket,[socket,bytes=QByteArray{}]() mutable {
                bytes+=socket->readAll(); const int split=bytes.indexOf("\r\n\r\n");
                if (split<0 || socket->property("sent").toBool()) return;
                int length=0; for (const auto &line:bytes.left(split).split('\n')) if(line.toLower().startsWith("content-length:")) length=line.mid(15).trimmed().toInt();
                if (bytes.size()<split+4+length) return; socket->setProperty("sent",true);
                const auto request = QJsonDocument::fromJson(bytes.mid(split + 4, length)).object();
                QImage sent;
                assert(sent.loadFromData(QByteArray::fromBase64(request["image_png"].toString().toLatin1())));
                assert(sent.width() > 200 && sent.height() > 100);
                const QJsonObject result{{"protocol","yagf-layout/1"},{"model","fixture-layout"},{"width",sent.width()},{"height",sent.height()},
                    {"areas",QJsonArray{QJsonObject{{"id","text"},{"label","text"},{"type","text"},{"score",.9},{"box",QJsonArray{10,20,60,15}}},
                                        QJsonObject{{"id","image"},{"label","chart"},{"type","picture"},{"score",.95},{"box",QJsonArray{80,40,60,45}}}}}};
                const auto data=QJsonDocument(result).toJson(QJsonDocument::Compact);
                socket->write("HTTP/1.1 200 OK\r\nContent-Length: "+QByteArray::number(data.size())+"\r\nConnection: close\r\n\r\n"+data); socket->disconnectFromHost();
            });
        });
        QSettings("YAGF","YAGF Revival").setValue("layout/url",QString("http://127.0.0.1:%1").arg(server.serverPort()));
        yagf::core::OcrDocument book; book.sourcePath=sourcePath; book.engine="tesseract"; book.language="eng";
        yagf::core::OcrPage page; page.width=page.sourceWidth=200; page.height=page.sourceHeight=100; page.language="eng";
        for (int line = 0; line < 3; ++line)
            for (int word = 0; word < 4; ++word)
                page.words.append({1,1,1,line+1,{10+45*word,20+20*line+2*word,25,8},"keep","keep",90,true});
        page.rebuildHierarchy();
        page.imageRegions={{"manual",{0,0,20,15},"eng",false,"text",false},{"old-auto",{110,0,80,15},{},false,"text",true}};
        book.replacePage(page); const auto project=directory.filePath("ai-layout.yagf.json"); QString error;
        assert(yagf::core::ProjectFile::save(project,book,&error));
        yagf::ui::MainWindow layoutWindow;
        layoutWindow.show(); layoutWindow.loadProject(project);
        auto *action=layoutWindow.findChild<QAction *>("aiPageAreasAction"); assert(action);
        auto *blocks=layoutWindow.findChild<QListWidget *>("imageRegions");
        waitUi([&]{return action->isEnabled() && blocks->count()==2;});
        auto *options=layoutWindow.findChild<QToolButton *>("smartSplitOptions"); assert(options && options->menu()->actions().contains(action));
        QTimer::singleShot(0, &layoutWindow, [&] {
            auto *dialog = qobject_cast<yagf::ui::AiPageLayoutDialog *>(QApplication::activeModalWidget());
            assert(dialog && dialog->findChild<QLabel *>("layoutSubtitle")->text().contains("deskews"));
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Cancel)->click();
        });
        action->trigger();
        const auto savedPage = [&] {
            for (auto *a : layoutWindow.findChildren<QAction *>()) if (a->text()=="Save project") a->trigger();
            yagf::core::OcrDocument saved;
            assert(yagf::core::ProjectFile::load(project, &saved, &error));
            return *saved.page(1);
        };
        const auto cancelled = savedPage();
        assert(cancelled.preprocessing.deskewDegrees == 0 && cancelled.words[0].bounds == page.words[0].bounds);
        QTimer::singleShot(0,&layoutWindow,[&] {
            auto *dialog=qobject_cast<yagf::ui::AiPageLayoutDialog *>(QApplication::activeModalWidget()); assert(dialog);
            assert(blocks->count()==2);
            dialog->findChild<QPushButton *>("analyzePageAreas")->click();
            auto *apply=dialog->findChild<QPushButton *>("applyPageAreas"); waitUi([&]{return apply->isEnabled();});
            assert(blocks->count()==2); apply->click();
        });
        action->trigger(); assert(blocks->count()==3);
        auto *words=layoutWindow.findChild<QListWidget *>("recognizedWords"); assert(words->count()==page.words.size());
        QAction *undo=nullptr, *redo=nullptr;
        for (auto *a:layoutWindow.findChildren<QAction *>()) {
            if (a->shortcut()==QKeySequence::Undo) undo=a;
            if (a->shortcut()==QKeySequence::Redo) redo=a;
        }
        assert(undo && redo); undo->trigger(); assert(blocks->count()==2); assert(savedPage().preprocessing.deskewDegrees==0); redo->trigger(); assert(blocks->count()==3);
        const auto saved = savedPage();
        assert(saved.preprocessing.deskewDegrees > 2 && saved.words[0].id == page.words[0].id);
        assert(saved.layoutTraining.examples.isEmpty());
        assert(saved.words[0].bounds != page.words[0].bounds);
        assert(saved.imageRegions[0].id=="manual" && saved.words[0].text=="keep" && saved.words[0].reviewed);
        assert(saved.imageRegions[2].content=="picture" && saved.imageRegions[2].notText && saved.imageRegions[2].detectedLabel=="chart");
    }

    // AI splitting sends only the selected crop, reviews before replacing it,
    // preserves outside blocks/language/text, and translates crop coordinates.
    {
        QTcpServer server;assert(server.listen(QHostAddress::LocalHost));QVector<QSize> sentSizes;
        QObject::connect(&server,&QTcpServer::newConnection,&server,[&] {
            auto *socket=server.nextPendingConnection();QObject::connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
            QObject::connect(socket,&QTcpSocket::readyRead,socket,[&,socket,bytes=QByteArray{}]() mutable {
                bytes+=socket->readAll();const int split=bytes.indexOf("\r\n\r\n");if(split<0 || socket->property("sent").toBool())return;
                int length=0;for(const auto &line:bytes.left(split).split('\n'))if(line.toLower().startsWith("content-length:"))length=line.mid(15).trimmed().toInt();
                if(bytes.size()<split+4+length)return;socket->setProperty("sent",true);
                const auto request=QJsonDocument::fromJson(bytes.mid(split+4,length)).object();QImage crop;
                crop.loadFromData(QByteArray::fromBase64(request["image_png"].toString().toLatin1()),"PNG");sentSizes.append(crop.size());
                const QJsonObject result{{"protocol","yagf-layout/1"},{"model","crop-layout"},{"width",crop.width()},{"height",crop.height()},
                    {"areas",QJsonArray{QJsonObject{{"id","text"},{"label","text"},{"type","text"},{"score",.9},{"box",QJsonArray{5,5,60,20}}},
                                        QJsonObject{{"id","formula"},{"label","display_formula"},{"type","math"},{"score",.95},{"box",QJsonArray{80,32,50,25}}}}}};
                const auto data=QJsonDocument(result).toJson(QJsonDocument::Compact);
                socket->write("HTTP/1.1 200 OK\r\nContent-Length: "+QByteArray::number(data.size())+"\r\nConnection: close\r\n\r\n"+data);socket->disconnectFromHost();
            });
        });
        QSettings("YAGF","YAGF Revival").setValue("layout/url",QString("http://127.0.0.1:%1").arg(server.serverPort()));
        yagf::core::OcrDocument book;book.sourcePath=sourcePath;book.language="eng";book.engine="tesseract";
        yagf::core::OcrPage page;page.width=page.sourceWidth=200;page.height=page.sourceHeight=100;page.language="eng";
        page.imageRegions={{"outside",{0,0,30,15},"eng",false,"text",false},{"parent",{40,20,140,70},"rus",false,"mixed",true}};
        page.words.append({1,1,1,1,{45,25,40,15},"reviewed","reviewed",95,true});page.rebuildHierarchy();book.replacePage(page);
        const auto project=directory.filePath("crop-split.yagf.json");QString error;assert(yagf::core::ProjectFile::save(project,book,&error));
        yagf::ui::MainWindow splitWindow;splitWindow.show();splitWindow.loadProject(project);
        auto *blocks=splitWindow.findChild<QListWidget *>("imageRegions");auto *action=splitWindow.findChild<QAction *>("aiSplitBlockAction");assert(action);
        waitUi([&]{return blocks->count()==2 && splitWindow.findChild<QAction *>("aiPageAreasAction")->isEnabled();});assert(!action->isEnabled());
        blocks->setCurrentRow(1);assert(action->isEnabled());
        const auto review=[&](bool apply) {
            QTimer::singleShot(0,&splitWindow,[&,apply] {
                auto *dialog=qobject_cast<yagf::ui::AiPageLayoutDialog *>(QApplication::activeModalWidget());assert(dialog && dialog->windowTitle()=="AI split selected block");
                dialog->findChild<QPushButton *>("analyzePageAreas")->click();auto *accept=dialog->findChild<QPushButton *>("applyPageAreas");waitUi([&]{return accept->isEnabled();});
                assert(blocks->count()==2);
                if (apply) {
                    auto *type = dialog->findChild<QComboBox *>("layoutType1");
                    type->setCurrentIndex(type->findData("mixed"));
                    accept->click();
                } else dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Cancel)->click();
            });action->trigger();
        };
        review(false);assert(blocks->count()==2 && action->isEnabled());review(true);assert(blocks->count()==3);
        assert(sentSizes.size()==2);for(const auto &size:sentSizes)assert(size==QSize(140,70));
        const auto save=[&]{for(auto *a:splitWindow.findChildren<QAction *>())if(a->text()=="Save project")a->trigger();};
        save();yagf::core::OcrDocument saved;assert(yagf::core::ProjectFile::load(project,&saved,&error));
        const auto *edited=saved.page(1);assert(edited->imageRegions[0].id=="outside" && edited->imageRegions[0].bounds==page.imageRegions[0].bounds);
        assert(edited->imageRegions[1].bounds==QRect(45,25,60,20) && edited->imageRegions[2].bounds==QRect(120,52,50,25));
        assert(edited->layoutTraining.examples.size() == 2);
        const auto &example = edited->layoutTraining.examples.last();
        assert(example.scope == page.imageRegions[1].bounds);
        assert(example.proposed.last().toObject()["content"] == "math");
        assert(example.corrected.last().toObject()["content"] == "mixed");
        assert(example.corrected.last().toObject()["box"].toArray() == QJsonArray({120,52,50,25}));
        assert(edited->imageRegions[1].language=="rus" && edited->imageRegions[2].language=="rus");assert(edited->words[0].text=="reviewed" && edited->words[0].reviewed);
        QAction *undo=nullptr,*redo=nullptr;for(auto *a:splitWindow.findChildren<QAction *>()) {if(a->shortcut()==QKeySequence::Undo)undo=a;if(a->shortcut()==QKeySequence::Redo)redo=a;}
        assert(undo && redo);undo->trigger();assert(blocks->count()==2);save();assert(yagf::core::ProjectFile::load(project,&saved,&error));assert(saved.page(1)->imageRegions[1].id=="parent" && saved.page(1)->layoutTraining.examples.isEmpty());
        redo->trigger();assert(blocks->count()==3);save();
        blocks->setCurrentRow(2);
        splitWindow.findChild<QAction *>("imageRemoveBlockAction")->trigger();
        assert(blocks->count() == 2);
        save();
        assert(yagf::core::ProjectFile::load(project, &saved, &error));
        assert(saved.page(1)->layoutTraining.examples.size() == 2);
        for (const auto &sample : saved.page(1)->layoutTraining.examples)
            assert(sample.corrected.size() == 2);
    }

    // Formula growth edits OCR crops as one undoable page edit.
    {
        QImage scan(160,100,QImage::Format_RGB32);
        scan.fill(Qt::white);
        QPainter ink(&scan);
        ink.fillRect(58,48,9,23,Qt::black);
        ink.fillRect(60,36,5,7,Qt::black);
        ink.fillRect(20,15,7,11,Qt::black);
        ink.end();
        const auto scanPath = directory.filePath("limits.png");
        assert(scan.save(scanPath));
        yagf::core::OcrDocument book;
        book.sourcePath = scanPath;
        yagf::core::OcrPage page;
        page.width = page.sourceWidth = scan.width();
        page.height = page.sourceHeight = scan.height();
        page.imageRegions = {{"upper",{0,0,130,45},"rus",false,"text"},
                             {"formula",{40,45,64,30},"",false,"math"}};
        page.words.append({1,1,1,1,{20,15,7,11},"x","x",99,true});
        page.rebuildHierarchy();
        book.replacePage(page);
        const auto project = directory.filePath("limits.yagf.json");
        assert(yagf::core::ProjectFile::save(project,book,&error));
        yagf::ui::MainWindow window;
        window.loadProject(project);
        auto *refine = window.findChild<QAction *>("refineFormulaBlocksAction");
        assert(refine);
        waitUi([&]{return refine->isEnabled();});
        refine->trigger();
        auto savedFormula = [&] {
            for (auto *a : window.findChildren<QAction *>())
                if (a->text()=="Save project") a->trigger();
            yagf::core::OcrDocument saved;
            assert(yagf::core::ProjectFile::load(project,&saved,&error));
            assert(saved.page(1)->words[0].reviewed);
            return saved.page(1)->imageRegions[1].bounds;
        };
        assert(savedFormula().top()==36);
        for (auto *a : window.findChildren<QAction *>())
            if (a->shortcut()==QKeySequence::Undo) a->trigger();
        assert(savedFormula()==page.imageRegions[1].bounds);
    }

    // Companion readings stay separate until an explicit review choice. The
    // real window applies that choice through Undo and saves its provenance.
    {
        yagf::core::OcrDocument compared; compared.sourcePath=sourcePath; compared.language="eng"; compared.engine="tesseract";
        yagf::core::OcrPage page;page.width=page.sourceWidth=200;page.height=page.sourceHeight=100;page.language="eng";page.engineId="tesseract";
        page.words.append({1,1,1,1,{10,20,30,12},"mod","mod",90});page.words.append({1,1,1,1,{41,20,70,12},"ernization","ernization",90});page.rebuildHierarchy();
        yagf::core::OcrReading reading;reading.source="test-companion";reading.engine="doctr-remote";reading.label="Independent OCR";reading.language="eng";reading.area={0,0,200,100};reading.items.append({{10,20,101,12},"modernization",1,1,1,false});page.ocrReadings.append(reading);compared.replacePage(page);
        const auto project=directory.filePath("comparison.yagf.json");assert(yagf::core::ProjectFile::save(project,compared,&error));
        yagf::ui::MainWindow comparedWindow;comparedWindow.loadProject(project);
        auto *words=comparedWindow.findChild<QListWidget *>("recognizedWords");assert(words->count()==2);
        auto *review=comparedWindow.findChild<QAction *>("ocrReadingsAction");assert(review && review->isEnabled());
        QTimer::singleShot(0,&comparedWindow,[&] {
            auto *dialog=qobject_cast<QDialog *>(QApplication::activeModalWidget());assert(dialog && dialog->objectName()=="ocrReadingsDialog");
            auto *use=dialog->findChild<QPushButton *>("useOcrReading");assert(use && use->isEnabled());use->click();
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
        });
        review->trigger();assert(words->count()==1 && words->item(0)->text()=="modernization");
        QAction *undo=nullptr,*redo=nullptr;
        for (auto *a:comparedWindow.findChildren<QAction *>()) {if(a->shortcut()==QKeySequence::Undo)undo=a;if(a->shortcut()==QKeySequence::Redo)redo=a;}
        assert(undo && redo);undo->trigger();assert(words->count()==2);redo->trigger();assert(words->count()==1);
        for(auto *a:comparedWindow.findChildren<QAction *>())if(a->text()=="Save project")a->trigger();
        yagf::core::OcrDocument saved;assert(yagf::core::ProjectFile::load(project,&saved,&error));assert(saved.page(1)->ocrReadings.size()==1);
        assert(saved.page(1)->words[0].reviewed && saved.page(1)->words[0].readingEngine=="Independent OCR");
    }

    // A failed OCR job closes progress before reporting the error.
    helperScript("tesseract", "#!/bin/sh\necho 'OCR failure fixture' >&2\nexit 1\n");
    yagf::ui::MainWindow failedRecognitionWindow;
    failedRecognitionWindow.loadSource(sourcePath);
    bool sawRecognitionError = false;
    QTimer errorDialogTimer;
    QObject::connect(&errorDialogTimer, &QTimer::timeout, &failedRecognitionWindow, [&] {
        auto *errorDialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        if (!errorDialog) return;
        auto *progress = failedRecognitionWindow.findChild<QProgressDialog *>("recognitionProgressDialog");
        assert(progress && !progress->isVisible());
        sawRecognitionError = true;
        errorDialog->accept();
    });
    errorDialogTimer.start(10);
    failedRecognitionWindow.findChild<QAction *>("recognizeAllAction")->trigger();
    waitUi([&] { return sawRecognitionError; });
}

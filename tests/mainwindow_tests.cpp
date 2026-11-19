#include "mainwindow.h"
#include "projectfile.h"
#include "verificationcontroller.h"
#include "correctionretention.h"
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
#include <QGraphicsScene>
#include <QImage>
#include <QLineEdit>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
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
#include <QUrl>
#include <QTest>
#include <QSplitter>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QTemporaryDir directory;
    assert(directory.isValid());
    qputenv("XDG_DATA_HOME", QFile::encodeName(directory.path()));
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, directory.path());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
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

    yagf::ui::MainWindow window;
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
        assert((groups == QStringList{"Scans", "PDF", "Text and HTML", "OCR data"}));
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
        auto *ocrButton = window.findChild<QPushButton *>("primaryButton");
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
    QPushButton *recognize = nullptr, *apply = nullptr;
    for (QPushButton *button : window.findChildren<QPushButton *>()) {
        if (button->text() == QStringLiteral("Recognize")) recognize = button;
        if (button->text() == QStringLiteral("Apply correction")) apply = button;
    }
    assert(recognize && apply && recognize->isEnabled());
    assert(!recognize->icon().isNull());
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
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        assert(dialog);
        auto *list = dialog->findChild<QListWidget *>(QStringLiteral("availableOcrLanguages"));
        assert(list && list->count() == 2);
        for (int i = 0; i < list->count(); ++i)
            if (list->item(i)->text() == QStringLiteral("deu")) list->item(i)->setCheckState(Qt::Checked);
        choseLanguages = true;
        dialog->accept();
    });
    picker->trigger();
    assert(choseLanguages && language->text() == QStringLiteral("eng+deu"));
    QAction *undo = nullptr, *redo = nullptr;
    for (QAction *action : window.findChildren<QAction *>()) {
        if (action->text() == QStringLiteral("Undo")) undo = action;
        if (action->text() == QStringLiteral("Redo")) redo = action;
    }
    assert(undo && redo);
    auto *recognizeWholeDocument = window.findChild<QToolButton *>("recognizeAllButton");
    assert(recognizeWholeDocument && recognizeWholeDocument->isEnabled());
    recognizeWholeDocument->click();
    auto *singleProgress = window.findChild<QProgressDialog *>("recognitionProgressDialog");
    assert(singleProgress && singleProgress->isVisible());
    assert(singleProgress->value() == 0 && singleProgress->maximum() == 1);
    assert(singleProgress->labelText() == "0 / 1 pages recognized");
    QElapsedTimer timer;
    timer.start();
    while (words->count() != 2 && timer.elapsed() < 3000) {
        QApplication::processEvents();
        QThread::msleep(10);
    }
    assert(words->count() == 2);
    assert(!singleProgress->isVisible());
    QFile args(argsPath);
    assert(args.open(QIODevice::ReadOnly));
    assert(args.readAll().split('\n').contains("eng+deu"));
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
        assert(choice && choice->count() == 6);
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
    assert(!regionBox(0).intersects(regionBox(1)));
    assert(regionBox(0).right() + 1 == regionBox(1).left());
    regionUndo->trigger();
    assert(regionBox(0) == beforeCollision);
    drag({qreal(beforeCollision.center().x()), qreal(beforeCollision.center().y())},
         {180, qreal(beforeCollision.center().y())});
    assert(!regionBox(0).intersects(regionBox(1)));
    assert(regionBox(0).right() + 1 == regionBox(1).left());
    regionUndo->trigger();
    drag({95, 0}, {195, 80});
    assert(regions->count() == 3); // The new block keeps the permitted part of the drag.
    assert(!regionBox(2).intersects(regionBox(0)) && !regionBox(2).intersects(regionBox(1)));
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
    QPushButton *recognizeRegions = nullptr;
    for (QPushButton *button : regionWindow.findChildren<QPushButton *>())
        if (button->text() == QStringLiteral("Recognize")) recognizeRegions = button;
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
        assert(!saved.intersects(obstacle));
        assert(backwards ? saved.left() == obstacle.right() + 1 : saved.right() + 1 == obstacle.left());
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
    contextSave->trigger();
    yagf::core::OcrDocument contextualBook;
    assert(yagf::core::ProjectFile::load(bookPath, &contextualBook, &error));
    assert(contextualBook.context.description == "A Qt programming manual");
    assert(!contextualBook.context.inferFromRecognizedText);
    assert(contextualBook.preserveFormatting);
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
    resolutionWindow.findChild<QAction *>("showVerificationQueue")->trigger();
    auto *issues = resolutionWindow.findChild<QListView *>("verificationItems");
    assert(issues && issues->model()->rowCount() == 1);
    resolutionQueue->navigate(1);
    waitUi([&] { return resolutionView->scene()->sceneRect().width() == 1200; });
    assert(inheritedLanguage->text() == "eng");
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
    assert(resolutionLoaded.page(2)->words[0].reviewed && resolutionLoaded.page(2)->words[0].bounds.x() == 27);
    for (auto *action : resolutionWindow.findChildren<QAction *>())
        if (action->shortcut() == QKeySequence::Undo) action->trigger();
    waitUi([&] { return resolutionView->scene()->sceneRect().width() == 1200; });
    saveResolution();
    assert(yagf::core::ProjectFile::load(resolutionProject, &resolutionLoaded, &error));
    assert(resolutionLoaded.page(2)->renderDpi == 300 && !resolutionLoaded.page(2)->words[0].reviewed);

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
    auto *recognizeAll = batchResolutionWindow.findChild<QToolButton *>("recognizeAllButton");
    assert(recognizeAll && recognizeAll->isEnabled() && recognizeAll->text() == "Recognize all");
    recognizeAll->click();
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

    // The dialog cancels the same queue as the toolbar and can be reused.
    yagf::ui::MainWindow cancelBatchWindow;
    cancelBatchWindow.show(); cancelBatchWindow.loadSource(resolutionPdf);
    auto *cancelRecognizeAll = cancelBatchWindow.findChild<QToolButton *>("recognizeAllButton");
    waitUi([&] { return cancelRecognizeAll->isEnabled(); });
    cancelRecognizeAll->click();
    auto *cancelProgress = cancelBatchWindow.findChild<QProgressDialog *>("recognitionProgressDialog");
    assert(cancelProgress && cancelProgress->isVisible());
    auto *cancelProgressButton = cancelProgress->findChild<QPushButton *>();
    assert(cancelProgressButton && cancelProgressButton->text() == "Cancel");
    cancelProgressButton->click();
    waitUi([&] { return cancelRecognizeAll->isEnabled(); });
    assert(!cancelProgress->isVisible());
    cancelRecognizeAll->click();
    assert(cancelProgress->isVisible() && cancelProgress->value() == 0 && !cancelProgress->wasCanceled());
    waitUi([&] { return cancelRecognizeAll->isEnabled(); });
    assert(!cancelProgress->isVisible());

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
    failedRecognitionWindow.findChild<QToolButton *>("recognizeAllButton")->click();
    waitUi([&] { return sawRecognitionError; });
}

#include "mainwindow.h"
#include "projectfile.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QElapsedTimer>
#include <QFile>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QImage>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QThread>
#include <QTimer>
#include <QTest>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QTemporaryDir directory;
    assert(directory.isValid());
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
        "printf '%s\\n' \"$@\" > \"$YAGF_TEST_ARGS\"\ncat \"$YAGF_TEST_TSV\"\n";
    assert(script.write(body) == body.size());
    script.close();
    assert(script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    qputenv("PATH", QFile::encodeName(directory.path()) + ':' + qgetenv("PATH"));
    qputenv("YAGF_TEST_TSV", QFile::encodeName(fixturePath));
    qputenv("YAGF_TEST_ARGS", QFile::encodeName(argsPath));

    yagf::ui::MainWindow window;
    window.loadSource(sourcePath);
    auto *words = window.findChild<QListWidget *>(QStringLiteral("recognizedWords"));
    auto *editor = window.findChild<QTextEdit *>(QStringLiteral("recognizedTextEditor"));
    assert(words && editor);
    QPushButton *recognize = nullptr, *apply = nullptr;
    for (QPushButton *button : window.findChildren<QPushButton *>()) {
        if (button->text() == QStringLiteral("Recognize")) recognize = button;
        if (button->text() == QStringLiteral("Apply correction")) apply = button;
    }
    assert(recognize && apply && recognize->isEnabled());
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
    recognize->click();
    QElapsedTimer timer;
    timer.start();
    while (words->count() != 2 && timer.elapsed() < 3000) {
        app.processEvents();
        QThread::msleep(10);
    }
    assert(words->count() == 2);
    QFile args(argsPath);
    assert(args.open(QIODevice::ReadOnly));
    assert(args.readAll().split('\n').contains("eng+deu"));
    auto *review = window.findChild<QPushButton *>(QStringLiteral("reviewWordButton"));
    assert(review);
    words->setCurrentRow(0);
    app.processEvents();
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
    regionWindow.loadSource(sourcePath);
    regionWindow.show();
    app.processEvents();
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
        app.processEvents();
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
    QPushButton *recognizeRegions = nullptr;
    for (QPushButton *button : regionWindow.findChildren<QPushButton *>())
        if (button->text() == QStringLiteral("Recognize regions")) recognizeRegions = button;
    assert(recognizeRegions);
    recognizeRegions->click();
    timer.restart();
    while (regionWords->count() != 2 && timer.elapsed() < 5000) {
        app.processEvents();
        QThread::msleep(10);
    }
    assert(regionWords->count() == 2);
    QTest::mouseClick(pageView->viewport(), Qt::LeftButton, Qt::ControlModifier,
                      pageView->mapFromScene(QPointF(30, 35)));
    auto *regionEditor = regionWindow.findChild<QTextEdit *>(QStringLiteral("recognizedTextEditor"));
    assert(regionEditor && regionEditor->toPlainText() == QStringLiteral("mod"));
    // Opening and dismissing a book suggestion must leave the target untouched.
    yagf::core::OcrDocument book;
    book.sourcePath = sourcePath;
    yagf::core::OcrPage page;
    page.width = page.sourceWidth = image.width();
    page.height = page.sourceHeight = image.height();
    page.words.append({1, 1, 1, 1, QRect(5, 5, 40, 15), "QWldget", "QWidget", 75, true});
    page.words.append({1, 1, 1, 1, QRect(60, 5, 40, 15), "QWldget", "QWldget", 99});
    book.replacePage(page);
    const QString bookPath = directory.filePath("book.yagf.json");
    QString error;
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

}

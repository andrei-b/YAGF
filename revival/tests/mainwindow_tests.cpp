#include "mainwindow.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QThread>
#include <QTimer>

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
    QFile script(scriptPath);
    assert(script.open(QIODevice::WriteOnly));
    const QByteArray body = "#!/bin/sh\ncat \"$YAGF_TEST_TSV\"\n";
    assert(script.write(body) == body.size());
    script.close();
    assert(script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    qputenv("PATH", QFile::encodeName(directory.path()) + ':' + qgetenv("PATH"));
    qputenv("YAGF_TEST_TSV", QFile::encodeName(fixturePath));

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
        assert(choice && choice->count() == 3);
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
}

#include "formulaeditor.h"
#include "detail/pdfformulas.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QFutureWatcher>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrentRun>
#include <memory>

namespace yagf::ui {

std::optional<QString> editFormulaSource(QWidget *parent, const QString &source, bool display)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(QObject::tr("Edit formula"));
    dialog.setObjectName("formulaEditor");
    dialog.resize(620, 350);
    auto *layout = new QVBoxLayout(&dialog);
    auto *sourceLabel = new QLabel(QObject::tr("&LaTeX"), &dialog);
    auto *editor = new QPlainTextEdit(source, &dialog);
    editor->setObjectName("formulaSource");
    editor->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    sourceLabel->setBuddy(editor);
    layout->addWidget(sourceLabel);
    layout->addWidget(editor);
    layout->addWidget(new QLabel(QObject::tr("Preview"), &dialog));
    auto *scroll = new QScrollArea(&dialog);
    scroll->setWidgetResizable(true);
    auto *preview = new QLabel(scroll);
    preview->setObjectName("formulaPreview");
    preview->setAlignment(Qt::AlignCenter);
    preview->setTextFormat(Qt::PlainText);
    preview->setWordWrap(true);
    scroll->setWidget(preview);
    layout->addWidget(scroll, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    const bool available = !QStandardPaths::findExecutable("pdflatex").isEmpty() &&
                           !QStandardPaths::findExecutable("pdftoppm").isEmpty();
    std::shared_ptr<std::atomic_bool> running;
    QTimer debounce;
    debounce.setSingleShot(true);
    debounce.setInterval(250);
    QObject::connect(&debounce, &QTimer::timeout, &dialog, [&] {
        const auto text = editor->toPlainText();
        auto cancel = std::make_shared<std::atomic_bool>(false);
        running = cancel;
        using Result = std::expected<QImage, QString>;
        auto *watcher = new QFutureWatcher<Result>(&dialog);
        QObject::connect(watcher, &QFutureWatcher<Result>::finished, &dialog, [preview, watcher, cancel] {
            if (!cancel->load()) {
                const auto result = watcher->result();
                if (result) preview->setPixmap(QPixmap::fromImage(*result));
                else preview->setText(result.error());
            }
            watcher->deleteLater();
        });
        watcher->setFuture(QtConcurrent::run([text, display, cancel] {
            return processing::detail::renderFormulaPreview(text, display, *cancel);
        }));
    });
    const auto update = [&] {
        if (running) running->store(true);
        debounce.stop();
        const bool hasText = !editor->toPlainText().trimmed().isEmpty();
        buttons->button(QDialogButtonBox::Save)->setEnabled(hasText);
        if (!available) preview->setText(QObject::tr("For a live preview, install pdfLaTeX and pdftoppm. You can still edit and save the LaTeX."));
        else if (!hasText) preview->clear();
        else {
            preview->setText(QObject::tr("Rendering…"));
            debounce.start();
        }
    };
    QObject::connect(editor, &QPlainTextEdit::textChanged, &dialog, update);
    update();
    editor->setFocus();
    const auto accepted = dialog.exec() == QDialog::Accepted;
    if (running) running->store(true);
    if (accepted && !editor->toPlainText().trimmed().isEmpty()) return editor->toPlainText();
    return {};
}

}

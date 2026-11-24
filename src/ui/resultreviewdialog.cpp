#include "resultreviewdialog.h"
#include "formulatext.h"
#include <QBuffer>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace yagf::ui {
ResultReviewDialog::ResultReviewDialog(const core::OcrPage &source, QVector<int> words, QImage scan,
    bool unicodeOutput, const QString &sessionToken, QWidget *parent)
    : QDialog(parent)
    , page(source)
    , image(std::move(scan))
    , unicode(unicodeOutput)
    , batches(intelligence::reviewBatches(page, words))
{
    setObjectName("aiResultReview");
    setWindowTitle(tr("Review OCR result with AI"));
    resize(820, 660);
    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout;
    QSettings settings("YAGF", "YAGF Revival");
    endpoint = new QLineEdit(
        settings.value("ai/endpoint", "http://127.0.0.1:8080/v1/chat/completions").toString(), this);
    model = new QLineEdit(settings.value("ai/model").toString(), this);
    token = new QLineEdit(sessionToken, this);
    token->setEchoMode(QLineEdit::Password);
    endpoint->setObjectName("aiEndpoint");
    model->setObjectName("aiModel");
    form->addRow(tr("Chat-completions endpoint"), endpoint);
    form->addRow(tr("Model"), model);
    form->addRow(tr("Session token"), token);
    layout->addLayout(form);
    withImage = new QCheckBox(tr("Include the scan crop (vision models)"), this);
    withImage->setEnabled(!image.isNull() && image.size() == QSize(page.width, page.height));
    layout->addWidget(withImage);
    if (words.size() == 1 && words.first() >= 0 && words.first() < page.words.size()) {
        const int i = words.first();
        QStringList context;
        const auto &target = page.words[i];
        for (int j = std::max(0, i - 4); j <= std::min(int(page.words.size()) - 1, i + 4); ++j) {
            const auto &w = page.words[j];
            if (w.block == target.block && w.paragraph == target.paragraph && w.line == target.line)
                context.append(j == i ? QStringLiteral("[…]") : w.text.left(100));
        }
        if (context.size() > 1)
            nearby = context.join(' ');
    }
    auto *sourceText = new QPlainTextEdit(this);
    sourceText->setReadOnly(true);
    sourceText->setMaximumHeight(110);
    QStringList lines;
    int last = -1;
    for (const auto &batch : batches)
        for (const auto &part : batch) {
            if (last == part.word)
                continue;
            const auto &w = page.words[part.word];
            if (last < 0 || w.layoutBlock || page.words[last].layoutBlock || w.block != page.words[last].block
                || w.paragraph != page.words[last].paragraph || w.line != page.words[last].line
                || last + 1 != part.word)
                lines.append(QString());
            else
                lines.last() += ' ';
            lines.last() += core::formulaSourceText(w);
            last = part.word;
        }
    if (!nearby.isEmpty())
        lines.prepend(tr("Nearby text · %1").arg(nearby));
    sourceText->setPlainText(lines.join('\n'));
    layout->addWidget(sourceText);
    ask = new QPushButton(tr("Ask AI to review"), this);
    ask->setObjectName("askAiResult");
    layout->addWidget(ask);
    status = new QLabel(
        tr("Ask AI sends the OCR text shown above to the selected model in %1 request(s).").arg(batches.size()), this);
    status->setObjectName("aiResultStatus");
    status->setTextFormat(Qt::PlainText);
    status->setWordWrap(true);
    layout->addWidget(status);
    list = new QTreeWidget(this);
    list->setObjectName("aiResultChanges");
    list->setRootIsDecorated(false);
    list->setHeaderLabels({ tr("Current text"), tr("Suggested text"), tr("Reason") });
    list->header()->setSectionResizeMode(QHeaderView::Interactive);
    list->setColumnWidth(0, 230);
    list->setColumnWidth(1, 230);
    layout->addWidget(list);
    preview = new QLabel(this);
    preview->setAlignment(Qt::AlignCenter);
    layout->addWidget(preview);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    apply = buttons->addButton(tr("Apply checked changes"), QDialogButtonBox::ActionRole);
    apply->setObjectName("applyAiResult");
    apply->setEnabled(false);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(this, &QDialog::finished, this, [this] { chat.cancel(); });
    connect(list, &QTreeWidget::itemChanged, this, [this] { updateApply(); });
    connect(list, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) {
        preview->clear();
        if (!item || image.size() != QSize(page.width, page.height))
            return;
        const int row = list->indexOfTopLevelItem(item);
        if (row < 0 || row >= changes.size())
            return;
        const auto box = page.words[changes[row].word].bounds;
        if (!image.rect().contains(box))
            return;
        preview->setPixmap(QPixmap::fromImage(
            image.copy(box).scaled(QSize(700, 120), Qt::KeepAspectRatio, Qt::SmoothTransformation)));
    });
    connect(ask, &QPushButton::clicked, this, [this] {
        if (batches.isEmpty())
            return;
        if (usedEndpoint != endpoint->text().trimmed() || usedModel != model->text().trimmed()
            || usedImage != withImage->isChecked()) {
            batchIndex = 0;
            changes.clear();
            list->clear();
            preview->clear();
        }
        usedEndpoint = endpoint->text().trimmed();
        usedModel = model->text().trimmed();
        usedImage = withImage->isChecked();
        sendBatch();
    });
    connect(apply, &QPushButton::clicked, this, [this] {
        QVector<intelligence::ReviewChange> selected;
        for (int i = 0; i < changes.size(); ++i)
            if (list->topLevelItem(i)->checkState(0) == Qt::Checked)
                selected.append(changes[i]);
        if (selected.isEmpty())
            return;
        auto result = intelligence::applyChanges(page, selected, usedModel, unicode);
        if (!result) {
            status->setText(result.error());
            return;
        }
        edited = std::move(*result);
        accept();
    });
    ask->setEnabled(!batches.isEmpty());
}
QString ResultReviewDialog::sessionToken() const { return token->text(); }
void ResultReviewDialog::updateApply()
{
    bool selected = false;
    for (int i = 0; i < list->topLevelItemCount(); ++i)
        selected |= list->topLevelItem(i)->checkState(0) == Qt::Checked;
    apply->setEnabled(selected && endpoint->isEnabled());
}
void ResultReviewDialog::setWorking(bool active)
{
    for (auto *field : { endpoint, model, token })
        field->setEnabled(!active);
    withImage->setEnabled(!active && !image.isNull() && image.size() == QSize(page.width, page.height));
    ask->setEnabled(!active && batchIndex < batches.size());
    updateApply();
}
void ResultReviewDialog::sendBatch()
{
    if (batchIndex >= batches.size())
        return;
    const auto &batch = batches[batchIndex];
    QRect crop;
    for (const auto &part : batch)
        crop |= page.words[part.word].bounds;
    crop = crop.adjusted(-4, -4, 4, 4).intersected(image.rect());
    QJsonArray items;
    for (const auto &part : batch) {
        const auto &w = page.words[part.word];
        QJsonObject item { { "id", part.key }, { "text", part.text }, { "block", w.block }, { "line", w.line },
            { "paragraph", w.paragraph } };
        if (usedImage) {
            const auto box = w.bounds.translated(-crop.topLeft());
            item["box"] = QJsonArray { box.x(), box.y(), box.width(), box.height() };
        }
        items.append(item);
    }
    QString prompt
        = tr("Review this OCR transcription for likely recognition errors. Preserve the author's language, "
             "spelling, "
             "notation, layout and numbers. Check context and mathematical notation. Omit speculative changes. "
             "Return JSON {\"changes\":[{\"id\":\"source item id\",\"before\":\"exact original substring\","
             "\"after\":\"replacement\",\"reason\":\"brief evidence\"}]}. Within each item, quote enough "
             "original text "
             "to match exactly once. Return an empty changes array if nothing needs correction. ");
    if (usedImage)
        prompt += tr("The attached scan shows a %1 by %2 pixel crop. Boxes are x, y, width, height in those "
                     "original crop coordinates. ")
                      .arg(crop.width())
                      .arg(crop.height());
    if (!nearby.isEmpty())
        prompt += tr("Nearby text for context: %1. ").arg(nearby);
    prompt += QString::fromUtf8(QJsonDocument(items).toJson(QJsonDocument::Compact));
    QByteArray png;
    if (usedImage) {
        if (crop.isEmpty()) {
            status->setText(tr("This selection has no scan coordinates."));
            setWorking(false);
            return;
        }
        auto portion = image.copy(crop);
        if (portion.width() > 1500 || portion.height() > 1500)
            portion = portion.scaled(1500, 1500, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        if (!portion.save(&buffer, "PNG") || png.size() > 4 * 1024 * 1024) {
            status->setText(tr("The crop could not be sent. Try text-only review."));
            setWorking(false);
            return;
        }
    }
    setWorking(true);
    status->setText(tr("Reviewing batch %1 of %2…").arg(batchIndex + 1).arg(batches.size()));
    const auto sent = chat.send(
        usedEndpoint, usedModel, token->text(), prompt, png, 0, [this](ReviewChat::Answer reply) {
            if (!reply) {
                status->setText(reply.error());
                setWorking(false);
                return;
            }
            auto result = intelligence::readChanges(batches[batchIndex], *reply);
            if (!result) {
                status->setText(result.error());
                setWorking(false);
                return;
            }
            for (auto &change : *result) {
                changes.append(change);
                auto *item = new QTreeWidgetItem(list,
                    { change.before, change.after.isEmpty() ? tr("(delete)") : change.after, change.reason });
                item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
                item->setCheckState(0, Qt::Unchecked);
                item->setToolTip(0, page.words[change.word].id + "\n" + change.before);
                item->setToolTip(1, change.after);
                item->setToolTip(2, change.reason);
            }
            ++batchIndex;
            if (batchIndex < batches.size()) {
                sendBatch();
                return;
            }
            setWorking(false);
            status->setText(changes.isEmpty() ? tr("Review complete. The model suggested no changes.")
                                              : tr("Review complete. Check the changes you want to apply."));
            if (!changes.isEmpty())
                list->setCurrentItem(list->topLevelItem(0));
        });
    if (!sent) {
        status->setText(sent.error());
        setWorking(false);
        return;
    }
    QSettings settings("YAGF", "YAGF Revival");
    settings.setValue("ai/endpoint", usedEndpoint);
    settings.setValue("ai/model", usedModel);
}
}

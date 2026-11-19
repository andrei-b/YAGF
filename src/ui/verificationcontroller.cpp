#include "verificationcontroller.h"
#include <QAbstractListModel>
#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QHBoxLayout>
#include <QLabel>
#include <QListView>
#include <QMainWindow>
#include <QPushButton>
#include <QVBoxLayout>

namespace yagf::ui {
class VerificationListModel final : public QAbstractListModel {
public:
    using QAbstractListModel::QAbstractListModel;
    QVector<core::VerificationItem> items;
    int rowCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : items.size(); }
    QVariant data(const QModelIndex &index, int role) const override {
        if (!index.isValid() || index.row() >= items.size()) return {};
        const auto &item = items[index.row()];
        if (role == Qt::DisplayRole) return QStringLiteral("Page %1 · %2\n%3").arg(item.page).arg(core::verificationReasons(item.reasons), item.text.simplified().left(160));
        if (role == Qt::ToolTipRole) return item.text + '\n' + core::verificationReasons(item.reasons);
        return {};
    }
    void replace(QVector<core::VerificationItem> replacement) { beginResetModel(); items = std::move(replacement); endResetModel(); }
};
VerificationController::VerificationController(QMainWindow *window, const core::OcrDocument &doc)
    : QObject(window), document(doc), dock(new QDockWidget(tr("Verification queue"), window)),
      list(new QListView), model(new VerificationListModel(this)), reason(new QComboBox),
      pageOnly(new QCheckBox(tr("Current page only"))), counts(new QLabel)
{
    dock->setObjectName(QStringLiteral("verificationQueue"));
    auto *body = new QWidget;
    auto *layout = new QVBoxLayout(body);
    counts->setWordWrap(true);
    layout->addWidget(counts);
    reason->setObjectName(QStringLiteral("verificationFilter"));
    reason->addItem(tr("All reasons"), 0);
    for (unsigned flag : {core::LowConfidence, core::ConflictingReading, core::UnknownWord, core::GeometryAnomaly, core::PendingCorrection, core::UnrecognizedPage})
        reason->addItem(core::verificationReasons(flag), flag);
    layout->addWidget(reason);
    layout->addWidget(pageOnly);
    list->setObjectName(QStringLiteral("verificationItems"));
    list->setModel(model);
    list->setMinimumWidth(300);
    layout->addWidget(list);
    auto *navigation = new QHBoxLayout;
    auto *previous = new QPushButton(tr("Previous"));
    auto *next = new QPushButton(tr("Next"));
    previous->setToolTip(tr("Previous issue (Shift+F4)")); next->setToolTip(tr("Next issue (F4)"));
    navigation->addWidget(previous); navigation->addWidget(next); layout->addLayout(navigation);
    auto *review = new QPushButton(tr("Review / restore…"));
    auto *verify = new QPushButton(tr("Mark verified"));
    verify->setToolTip(tr("Accept the selected word's text and source box. Saved corrections must be restored or discarded."));
    auto *discard = new QPushButton(tr("Discard saved correction…"));
    layout->addWidget(review); layout->addWidget(verify); layout->addWidget(discard);
    dock->setWidget(body); window->addDockWidget(Qt::RightDockWidgetArea, dock); dock->hide();
    timer.setSingleShot(true); timer.setInterval(120);
    connect(&timer, &QTimer::timeout, this, &VerificationController::refresh);
    connect(dock, &QDockWidget::visibilityChanged, this, [this](bool visible) { if (visible) refresh(); });
    connect(reason, &QComboBox::currentIndexChanged, this, [this] { filter(); });
    connect(pageOnly, &QCheckBox::toggled, this, [this] { filter(); });
    connect(previous, &QPushButton::clicked, this, [this] { navigate(-1); });
    connect(next, &QPushButton::clicked, this, [this] { navigate(1); });
    connect(list, &QListView::clicked, this, [this] { if (!busy) if (auto item = selected()) emit activateRequested(*item); });
    connect(list, &QListView::doubleClicked, this, [this] { if (!busy) if (auto item = selected()) emit reviewRequested(*item); });
    connect(review, &QPushButton::clicked, this, [this] { if (auto item = selected()) emit reviewRequested(*item); });
    connect(verify, &QPushButton::clicked, this, [this] { if (auto item = selected(); item && !item->pending) emit verifyRequested(*item); });
    connect(discard, &QPushButton::clicked, this, [this] { if (auto item = selected(); item && item->pending) emit discardRequested(*item); });
}
void VerificationController::invalidate(int page)
{
    currentPage = page; dirty = true;
    if (dock->isVisible() && !busy) timer.start();
}
void VerificationController::setBusy(bool value)
{
    busy = value; dock->widget()->setEnabled(!busy);
    if (!busy && dirty && dock->isVisible()) timer.start();
}
void VerificationController::show() { dock->show(); dock->raise(); refresh(); }
void VerificationController::refresh()
{
    if (busy || !dirty || !dock->isVisible()) return;
    queue = core::buildVerificationQueue(document, [this](const core::OcrPage &page, const core::OcrWord &word, const QString &token) {
        QString language = page.language;
        for (const auto &region : page.imageRegions) if (!region.language.isEmpty() && region.bounds.contains(word.bounds.center())) { language = region.language; break; }
        spelling.setLanguages({language});
        return spelling.isMisspelled(token);
    });
    dirty = false;
    filter();
}
std::optional<core::VerificationItem> VerificationController::selected() const
{
    const int row = list->currentIndex().row();
    return row >= 0 && row < model->items.size() ? std::optional(model->items[row]) : std::nullopt;
}
void VerificationController::filter()
{
    auto old = selected();
    const int oldRow = list->currentIndex().row();
    QVector<core::VerificationItem> visible;
    const unsigned mask = reason->currentData().toUInt();
    for (const auto &item : queue.items)
        if ((!mask || (item.reasons & mask)) && (!pageOnly->isChecked() || item.page == currentPage)) visible.append(item);
    model->replace(std::move(visible));
    int row = -1;
    if (old) for (int i = 0; i < model->items.size(); ++i)
        if (model->items[i].page == old->page && model->items[i].wordId == old->wordId && model->items[i].pending == old->pending) { row = i; break; }
    if (row >= 0) list->setCurrentIndex(model->index(row));
    else if (oldRow >= 0) resumeRow = oldRow;
    counts->setText((queue.items.isEmpty() && queue.recognizedPages == document.totalPages ? tr("Review queue complete\n") : tr("Review outstanding\n")) + tr("%1 shown / %2 issues · %3/%4 pages recognized\n%5/%4 pages clear · %6/%7 words verified")
        .arg(model->items.size()).arg(queue.items.size()).arg(queue.recognizedPages).arg(document.totalPages)
        .arg(queue.completedPages).arg(queue.verifiedWords).arg(queue.totalWords));
}
void VerificationController::navigate(int direction)
{
    if (busy) return;
    show();
    if (model->items.isEmpty()) return;
    int row = list->currentIndex().row();
    row = row < 0 ? (direction > 0 ? resumeRow % model->items.size() : (resumeRow + model->items.size() - 1) % model->items.size()) : (row + direction + model->items.size()) % model->items.size();
    list->setCurrentIndex(model->index(row)); list->scrollTo(list->currentIndex());
    emit activateRequested(model->items[row]);
}
}

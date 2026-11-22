#include "verificationcontroller.h"
#include <QAbstractListModel>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMainWindow>
#include <QPainter>
#include <QPushButton>
#include <QShortcut>
#include <QStyledItemDelegate>
#include <algorithm>

namespace yagf::ui {
namespace {
enum Category { Issues, Formulas, ProblemBlocks, UncertainWords };
enum RowRole { TitleRole = Qt::UserRole+1, DetailRole, PageRole };

QString detail(const core::VerificationItem &item)
{
    QStringList parts;
    if (!item.label.isEmpty()) parts << item.label;
    if (item.reviewed) parts << QObject::tr("Verified");
    auto reasons=item.reasons;
    if ((reasons & core::LowConfidence) && item.confidence>=0) {
        parts << QObject::tr("Confidence %1%").arg(qRound(item.confidence));
        reasons &= ~core::LowConfidence;
    }
    if (reasons) parts << core::verificationReasons(reasons);
    return parts.join(QStringLiteral(" · "));
}

// Paint only visible rows. A book may have thousands of verification targets.
class NavigationRow final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem &option,const QModelIndex &) const override {
        return {180,option.fontMetrics.height()*2+22};
    }
    void paint(QPainter *p,const QStyleOptionViewItem &opt,const QModelIndex &index) const override {
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const auto area=opt.rect.adjusted(2,2,-2,-2);
        const bool selected=opt.state.testFlag(QStyle::State_Selected);
        const bool dark=opt.palette.color(QPalette::Base).lightness()<100;
        auto accent=opt.palette.color(QPalette::Highlight);
        auto fill=accent; fill.setAlpha(selected ? 32 : 10);
        p->setPen(selected ? accent : opt.palette.color(QPalette::Midlight));
        p->setBrush(fill);
        p->drawRoundedRect(area,6,6);
        const auto content=area.adjusted(10,5,-10,-5);
        const int half=content.height()/2;
        QFont title=opt.font; title.setWeight(QFont::DemiBold);
        p->setFont(title);
        const auto titleBox=QRect(content.x(),content.y(),content.width(),half);
        p->setPen(opt.palette.color(QPalette::Text));
        p->drawText(titleBox,Qt::AlignVCenter,p->fontMetrics().elidedText(index.data(TitleRole).toString(),Qt::ElideRight,titleBox.width()));
        QFont small=opt.font;
        if (small.pointSizeF()>0) small.setPointSizeF(std::max(8.,small.pointSizeF()-1));
        p->setFont(small);
        const auto meta=QRect(content.x(),content.y()+half,content.width(),content.height()-half);
        const auto page=index.data(PageRole).toString();
        const int width=p->fontMetrics().horizontalAdvance(page)+12;
        const QRect badge(meta.x(),meta.y(),width,meta.height());
        p->setPen(Qt::NoPen);
        auto tint=accent; tint.setAlpha(dark ? 50 : 24);
        p->setBrush(tint); p->drawRoundedRect(badge,4,4);
        p->setPen(dark ? opt.palette.color(QPalette::Text) : QColor(40,82,126));
        p->drawText(badge,Qt::AlignCenter,page);
        const auto rest=meta.adjusted(width+7,0,0,0);
        p->setPen(opt.palette.color(QPalette::Text));
        p->drawText(rest,Qt::AlignVCenter,p->fontMetrics().elidedText(index.data(DetailRole).toString(),Qt::ElideRight,rest.width()));
        p->restore();
    }
};
}
class VerificationListModel final : public QAbstractListModel {
public:
    using QAbstractListModel::QAbstractListModel;
    QVector<core::VerificationItem> items;
    int rowCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : items.size(); }
    QVariant data(const QModelIndex &index, int role) const override {
        if (!index.isValid() || index.row() >= items.size()) return {};
        const auto &item = items[index.row()];
        if (role==TitleRole) return (item.formulaPreview.isEmpty() ? item.text : item.formulaPreview).simplified().left(200);
        if (role==DetailRole) return detail(item);
        const auto page=tr("Page %1").arg(item.page);
        if (role==PageRole) return page;
        if (role==Qt::ToolTipRole || role==Qt::DisplayRole || role==Qt::AccessibleTextRole)
            return page + " · " + detail(item) + '\n' + item.text.simplified().left(500);
        return {};
    }
    void replace(QVector<core::VerificationItem> replacement) { beginResetModel(); items = std::move(replacement); endResetModel(); }
};
VerificationController::VerificationController(QMainWindow *window, const core::OcrDocument &doc)
    : QObject(window), document(doc), dock(new QDockWidget(tr("Document navigator"), window)),
      list(new QListView), model(new VerificationListModel(this)), reason(new QComboBox),
      pageOnly(new QCheckBox(tr("Current page only"))), search(new QLineEdit), categories(new QButtonGroup(this)),
      empty(new QLabel), counts(new QLabel), previous(new QPushButton(tr("Previous"))), next(new QPushButton(tr("Next"))),
      review(new QPushButton), verify(new QPushButton(tr("Mark verified"))), discard(new QPushButton(tr("Discard saved correction…")))
{
    dock->setObjectName(QStringLiteral("verificationQueue"));
    auto *body = new QWidget;
    body->setObjectName("navigatorBody");
    auto *layout = new QVBoxLayout(body);
    layout->setContentsMargins(12,12,12,12);
    layout->setSpacing(8);
    auto *modes=new QGridLayout;
    modes->setSpacing(4);
    const QStringList names{tr("Review queue"),tr("Formulas"),tr("Problem blocks"),tr("Uncertain words")};
    for (int i=0;i<names.size();++i) {
        auto *button=new QPushButton(names[i]);
        button->setObjectName("navigatorCategory"+QString::number(i));
        button->setProperty("navigationCategory",true);
        button->setCheckable(true);
        categories->addButton(button,i);
        modes->addWidget(button,i/2,i%2);
    }
    categories->button(Issues)->setChecked(true);
    layout->addLayout(modes);
    search->setObjectName("navigatorSearch");
    search->setPlaceholderText(tr("Search results…"));
    search->setAccessibleName(tr("Search navigation results"));
    search->setClearButtonEnabled(true);
    layout->addWidget(search);
    reason->setObjectName(QStringLiteral("verificationFilter"));
    reason->setAccessibleName(tr("Review reason"));
    reason->addItem(tr("All reasons"), 0);
    for (unsigned flag : {core::LowConfidence, core::ConflictingReading, core::UnknownWord, core::GeometryAnomaly, core::PendingCorrection, core::UnrecognizedPage, core::MathReading})
        reason->addItem(core::verificationReasons(flag), flag);
    layout->addWidget(reason);
    auto *scope=new QHBoxLayout;
    counts->setObjectName("navigatorCount");
    scope->addWidget(counts,1); scope->addWidget(pageOnly);
    layout->addLayout(scope);
    list->setObjectName(QStringLiteral("verificationItems"));
    list->setAccessibleName(tr("Navigation results"));
    list->setModel(model);
    list->setItemDelegate(new NavigationRow(list));
    list->setUniformItemSizes(true);
    list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    list->setMinimumWidth(260);
    layout->addWidget(list,1);
    empty->setObjectName("navigatorEmpty");
    empty->setWordWrap(true); empty->setAlignment(Qt::AlignCenter);
    layout->addWidget(empty,1);
    auto *navigation = new QHBoxLayout;
    previous->setIcon(window->style()->standardIcon(QStyle::SP_ArrowBack));
    next->setIcon(window->style()->standardIcon(QStyle::SP_ArrowForward));
    previous->setToolTip(tr("Previous result (Shift+F4)")); next->setToolTip(tr("Next result (F4)"));
    navigation->addWidget(previous); navigation->addWidget(next); layout->addLayout(navigation);
    auto *actions = new QHBoxLayout;
    actions->addWidget(review); actions->addWidget(verify);
    layout->addLayout(actions); layout->addWidget(discard);
    dock->setWidget(body); window->addDockWidget(Qt::RightDockWidgetArea, dock); dock->hide();
    timer.setSingleShot(true); timer.setInterval(120);
    connect(&timer, &QTimer::timeout, this, &VerificationController::refresh);
    connect(dock, &QDockWidget::visibilityChanged, this, [this](bool visible) {
        if (visible) { if (dirty) refresh(); else filter(); }
    });
    const auto changeFilter=[this]{filter(false);};
    connect(categories,&QButtonGroup::idClicked,this,changeFilter);
    connect(search,&QLineEdit::textChanged,this,changeFilter);
    connect(reason,&QComboBox::currentIndexChanged,this,changeFilter);
    connect(pageOnly,&QCheckBox::toggled,this,changeFilter);
    connect(previous, &QPushButton::clicked, this, [this] { navigate(-1); });
    connect(next, &QPushButton::clicked, this, [this] { navigate(1); });
    connect(list->selectionModel(),&QItemSelectionModel::currentChanged,this,[this]{updateActions();});
    const auto jump=[this]{ if (!busy) if (auto item=selected()) emit activateRequested(*item); };
    connect(list,&QListView::clicked,this,jump);
    connect(list,&QListView::doubleClicked,this,[this] {
        if (auto item=selected(); item && !busy) {
            if (review->isEnabled() && !review->isHidden()) emit reviewRequested(*item);
            else emit activateRequested(*item);
        }
    });
    for (const auto key : {Qt::Key_Return,Qt::Key_Enter}) {
        auto *open=new QShortcut(QKeySequence(key),list);
        open->setContext(Qt::WidgetShortcut);
        connect(open,&QShortcut::activated,this,jump);
    }
    connect(search,&QLineEdit::returnPressed,this,[this]{navigate(1);});
    connect(review, &QPushButton::clicked, this, [this] { if (auto item = selected()) emit reviewRequested(*item); });
    connect(verify, &QPushButton::clicked, this, [this] { if (auto item = selected()) emit verifyRequested(*item); });
    connect(discard, &QPushButton::clicked, this, [this] { if (auto item = selected()) emit discardRequested(*item); });
}
void VerificationController::invalidate(int page)
{
    currentPage = page; dirty = true;
    if (dock->isVisible() && !busy) timer.start();
}
void VerificationController::setPage(int page)
{
    if (currentPage==page) return;
    currentPage=page;
    if (pageOnly->isChecked() && dock->isVisible()) filter();
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
    if (document.sourcePath.isEmpty() && document.pages.isEmpty()) queue={};
    else queue = core::buildVerificationQueue(document, [this](const core::OcrPage &page, const core::OcrWord &word, const QString &token) {
        QString language = page.language;
        for (const auto &region : page.imageRegions) if (!region.language.isEmpty() && region.shape().contains(word.bounds.center())) { language = region.language; break; }
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
void VerificationController::filter(bool preserveSelection)
{
    const auto old=preserveSelection ? selected() : std::nullopt;
    const int oldRow=list->currentIndex().row();
    const int mode=categories->checkedId();
    const auto &source=mode==Formulas ? queue.formulas : mode==ProblemBlocks ? queue.problemBlocks :
                       mode==UncertainWords ? queue.uncertainWords : queue.items;
    reason->setVisible(mode==Issues);
    const unsigned mask=mode==Issues ? reason->currentData().toUInt() : 0;
    const auto query=search->text().trimmed();
    QVector<core::VerificationItem> visible;
    for (const auto &item : source) {
        if ((mask && !(item.reasons & mask)) || (pageOnly->isChecked() && item.page!=currentPage)) continue;
        if (!query.isEmpty() && !item.text.contains(query,Qt::CaseInsensitive) &&
            !item.formulaPreview.contains(query,Qt::CaseInsensitive) &&
            !detail(item).contains(query,Qt::CaseInsensitive) && tr("Page %1").arg(item.page).compare(query,Qt::CaseInsensitive)!=0) continue;
        visible.append(item);
    }
    model->replace(std::move(visible));
    int row=-1;
    if (old) for (int i=0;i<model->items.size();++i) {
        const auto &item=model->items[i];
        if (item.page==old->page && item.wordId==old->wordId && item.pending==old->pending &&
            item.target==old->target && item.blockId==old->blockId && item.formulaStart==old->formulaStart) {row=i; break;}
    }
    if (row>=0) list->setCurrentIndex(model->index(row));
    else if (old && oldRow>=0) resumeRow=oldRow;
    else if (!preserveSelection) resumeRow.reset();
    const int count=model->rowCount();
    list->setVisible(count>0); empty->setVisible(!count);
    if (document.sourcePath.isEmpty() && document.pages.isEmpty()) empty->setText(tr("Open a document to browse its pages."));
    else if (!query.isEmpty() || pageOnly->isChecked() || mask) empty->setText(tr("No results match these filters."));
    else empty->setText(mode==Formulas ? tr("No formulas on the indexed pages.") :
                        mode==ProblemBlocks ? tr("No problem blocks found.") :
                        mode==UncertainWords ? tr("No uncertain words found.") : tr("Review queue complete."));
    counts->setText(count==1 ? tr("1 result") : tr("%1 results").arg(count));
    counts->setToolTip(tr("%1 of %2 pages recognized · %3 of %4 words verified")
        .arg(queue.recognizedPages).arg(document.totalPages).arg(queue.verifiedWords).arg(queue.totalWords));
    previous->setEnabled(count); next->setEnabled(count);
    updateActions();
}
void VerificationController::updateActions()
{
    const auto item=selected();
    const int mode=categories->checkedId();
    const bool formula=mode==Formulas;
    const bool word=item && !item->wordId.isEmpty();
    review->setText(formula ? tr("Edit formula…") : tr("Review / restore…"));
    review->setVisible(mode!=ProblemBlocks);
    review->setEnabled(word && (!formula || item->formulaStart>=0));
    verify->setVisible((mode==Issues || mode==UncertainWords) && (!item || !item->pending));
    discard->setVisible(word && item->pending);
    verify->setEnabled(word && !formula && !item->pending && !item->reviewed);
    discard->setEnabled(word && item->pending);
}
void VerificationController::navigate(int direction)
{
    if (busy) return;
    show();
    const int size=model->items.size();
    if (!size) return;
    const int current=list->currentIndex().row();
    int row;
    if (current>=0) row=(current+direction+size)%size;
    else if (resumeRow) row=(*resumeRow+(direction<0 ? size-1 : 0))%size;
    else if (direction>0) {
        const auto it=std::ranges::lower_bound(model->items,currentPage,{},&core::VerificationItem::page);
        row=it==model->items.end() ? 0 : int(it-model->items.begin());
    } else {
        const auto it=std::ranges::upper_bound(model->items,currentPage,{},&core::VerificationItem::page);
        row=it==model->items.begin() ? size-1 : int(it-model->items.begin()-1);
    }
    list->setCurrentIndex(model->index(row)); list->scrollTo(list->currentIndex());
    emit activateRequested(model->items[row]);
}
}

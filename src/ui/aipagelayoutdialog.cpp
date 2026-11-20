#include "aipagelayoutdialog.h"
#include "aipageicons.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QGraphicsSceneMouseEvent>
#include <QGraphicsView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStyledItemDelegate>
#include <QStyleOptionComboBox>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <algorithm>
#include <numeric>

namespace yagf::ui {
namespace {
class BlockCardDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    // Cards draw their own checkbox and selection. Do not let the native list
    // delegate draw a second checkbox or reserve space to its left.
    void paint(QPainter *,const QStyleOptionViewItem &,const QModelIndex &) const override {}
    void updateEditorGeometry(QWidget *editor,const QStyleOptionViewItem &option,const QModelIndex &) const override {
        editor->setGeometry(option.rect);
    }
};
class AreaTypeCombo final : public QComboBox {
public:
    using QComboBox::QComboBox;
protected:
    void paintEvent(QPaintEvent *event) override {
        QComboBox::paintEvent(event);
        QStyleOptionComboBox option; initStyleOption(&option);
        const QPoint center=style()->subControlRect(QStyle::CC_ComboBox,&option,QStyle::SC_ComboBoxArrow,this).center();
        QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(QColor("#65758b"),1.5,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));
        QPainterPath arrow; arrow.moveTo(center.x()-3,center.y()-1); arrow.lineTo(center.x(),center.y()+2); arrow.lineTo(center.x()+3,center.y()-1);
        painter.drawPath(arrow);
    }
};
QColor areaColor(const QString &type) {
    return type=="picture" ? QColor("#ae6d2e") : type=="math" ? QColor("#315dac") :
           type=="mixed" ? QColor("#7f4899") : QColor("#198737");
}
QString typeLabel(const QString &type) {
    return type=="picture" ? QObject::tr("Picture") : type=="math" ? QObject::tr("Formula") :
           type=="mixed" ? QObject::tr("Text + formulas") : QObject::tr("Text");
}
class AreaOutline final : public QGraphicsRectItem {
public:
    AreaOutline(QRect bounds,std::function<void()> clicked):QGraphicsRectItem(bounds),clicked(std::move(clicked)) {
        setAcceptedMouseButtons(Qt::LeftButton); setCursor(Qt::PointingHandCursor);
    }
protected:
    void mousePressEvent(QGraphicsSceneMouseEvent *event) override { event->accept(); clicked(); }
private:
    std::function<void()> clicked;
};
QToolButton *thumbnail(const QImage &image,QRect bounds,QWidget *parent)
{
    auto *button=new QToolButton(parent);
    button->setIcon(QIcon(QPixmap::fromImage(image.copy(bounds).scaled(144,106,Qt::KeepAspectRatio,Qt::SmoothTransformation))));
    button->setIconSize(QSize(144,106)); button->setFixedSize(152,114);
    button->setObjectName("layoutThumbnail"); button->setToolTip(QObject::tr("Inspect this block on the page"));
    return button;
}
}
AiPageLayoutDialog::AiPageLayoutDialog(QImage source,QVector<core::ImageRegion> blocks,QWidget *parent,int pageNumber,QString bearerToken)
    :QDialog(parent),image(std::move(source)),existing(std::move(blocks))
{
    setWindowTitle(tr("AI page areas")); setWindowIcon(aiPageAreasIcon()); setObjectName("aiPageLayoutDialog"); resize(1160,820);
    setStyleSheet(QStringLiteral(R"(
        QDialog#aiPageLayoutDialog { background: #f5f6f8; color: #24354b; }
        QLabel#layoutTitle { font-size: 22px; font-weight: 700; color: #24354b; }
        QLabel#layoutSubtitle, QLabel#layoutMessage, QLabel#layoutBlockScore { color: #65758b; }
        QLabel#layoutPanelTitle { color: #334962; font-weight: 700; }
        QLabel#layoutCount { color: #65758b; padding: 2px 8px; }
        QFrame#layoutOptions { background: #fffaf2; border: 1px solid #e5dfd5; border-radius: 8px; }
        QFrame#layoutPagePanel { background: #e8ebef; border: 1px solid #d8dfe8; border-radius: 10px; }
        QGraphicsView#layoutPreview { border: 0; background: transparent; }
        QListWidget#layoutAreas { border: 0; background: transparent; outline: none; }
        QListWidget#layoutAreas::item { background: transparent; border: 0; padding: 0; }
        QToolButton#layoutThumbnail { background: #fff; border: 1px solid #e1e6ee; border-radius: 6px; padding: 3px; }
        QToolButton#layoutAlternative { color: #175ca9; background: #edf5ff; border: 1px solid #d4e4f7; border-radius: 5px; padding: 4px; text-align: left; }
        QToolButton#layoutAlternative:hover { background: #deecfc; }
        QToolButton#layoutOptionsButton, QToolButton#layoutFitPage, QToolButton#layoutZoomIn, QToolButton#layoutZoomOut {
            color: #334962; background: #ffffff; border: 1px solid #ced9e6; border-radius: 6px; padding: 5px 9px;
        }
        QComboBox { color: #334962; background: #ffffff; border: 1px solid #ced9e6; border-radius: 5px; padding: 4px 6px; }
        QComboBox::drop-down { border: 0; border-left: 1px solid #e1e6ee; width: 20px; background: #f4f7fb; }
        QPushButton { color: #334962; background: white; border: 1px solid #ced9e6; border-radius: 6px; padding: 7px 12px; }
        QPushButton:hover { background: #eef4fb; border-color: #9dbde2; }
        QPushButton#applyPageAreas { background: #216ec1; color: white; border: 1px solid #216ec1; border-radius: 6px; padding: 8px 18px; font-weight: 700; }
        QPushButton#applyPageAreas:disabled { background: #e7edf4; color: #a1acba; border-color: #e1e6ed; }
        QProgressBar { border: 0; background: #e1e8f1; max-height: 4px; }
        QProgressBar::chunk { background: #216ec1; }
    )"));
    auto *layout=new QVBoxLayout(this); layout->setContentsMargins(20,18,20,16); layout->setSpacing(12);
    auto *header=new QHBoxLayout;
    auto *wand=new QLabel(this); wand->setPixmap(aiPageAreasIcon().pixmap(36,36)); header->addWidget(wand);
    auto *titles=new QVBoxLayout;
    auto *title=new QLabel(pageNumber>0 ? tr("Page %1 · AI areas").arg(pageNumber) : tr("AI page areas"),this); title->setObjectName("layoutTitle"); titles->addWidget(title);
    auto *intro=new QLabel(tr("Review each block, choose its type, then apply the areas to your page."),this); intro->setObjectName("layoutSubtitle"); intro->setWordWrap(true); titles->addWidget(intro);
    header->addLayout(titles,1);
    optionsButton=new QToolButton(this); optionsButton->setText(tr("Options")); optionsButton->setCheckable(true); optionsButton->setArrowType(Qt::DownArrow); optionsButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon); optionsButton->setObjectName("layoutOptionsButton"); header->addWidget(optionsButton); layout->addLayout(header);
    auto *optionsPanel=new QFrame(this); optionsPanel->setObjectName("layoutOptions"); auto *form=new QFormLayout(optionsPanel); form->setContentsMargins(12,10,12,10);
    QSettings settings("YAGF","YAGF Revival");
    endpoint=new QLineEdit(settings.value("layout/url",QSettings().value("layout/url","http://127.0.0.1:8771")).toString(),this); endpoint->setObjectName("layoutEndpoint");
    token=new QLineEdit(std::move(bearerToken),this); token->setEchoMode(QLineEdit::Password); token->setPlaceholderText(tr("Optional; kept only for this session"));
    connect(endpoint,&QLineEdit::textEdited,token,&QLineEdit::clear);
    threshold=new QDoubleSpinBox(this); threshold->setObjectName("layoutThreshold"); threshold->setRange(.1,.95); threshold->setSingleStep(.05); threshold->setValue(settings.value("layout/threshold",QSettings().value("layout/threshold",.5)).toDouble());
    replaceAutomatic=new QCheckBox(tr("Replace automatically detected blocks; keep manual blocks"),this); replaceAutomatic->setChecked(true);
    form->addRow(tr("Layout service:"),endpoint); form->addRow(tr("Bearer token:"),token); form->addRow(tr("Minimum model score:"),threshold); form->addRow(replaceAutomatic);
    optionsPanel->hide(); layout->addWidget(optionsPanel);
    connect(optionsButton,&QToolButton::toggled,this,[this,optionsPanel](bool visible){optionsPanel->setVisible(visible); optionsButton->setArrowType(visible ? Qt::UpArrow : Qt::DownArrow);});
    auto *splitter=new QSplitter(this); splitter->setChildrenCollapsible(false);
    auto *pagePanel=new QFrame(splitter); pagePanel->setObjectName("layoutPagePanel"); auto *pageLayout=new QVBoxLayout(pagePanel); pageLayout->setContentsMargins(10,8,10,10);
    auto *pageTools=new QHBoxLayout; auto *pageTitle=new QLabel(tr("CURRENT PAGE"),pagePanel); pageTitle->setObjectName("layoutPanelTitle"); pageTools->addWidget(pageTitle); pageTools->addStretch();
    auto *fit=new QToolButton(pagePanel); fit->setText(tr("Fit page")); fit->setObjectName("layoutFitPage"); pageTools->addWidget(fit);
    auto *out=new QToolButton(pagePanel); out->setObjectName("layoutZoomOut"); out->setText(QStringLiteral("−")); out->setToolTip(tr("Zoom out")); pageTools->addWidget(out);
    auto *in=new QToolButton(pagePanel); in->setObjectName("layoutZoomIn"); in->setText("+"); in->setToolTip(tr("Zoom in")); pageTools->addWidget(in); pageLayout->addLayout(pageTools);
    scene=new QGraphicsScene(this); scene->addPixmap(QPixmap::fromImage(image)); scene->setSceneRect(image.rect()); previewTarget=image.rect();
    preview=new QGraphicsView(scene,pagePanel); preview->setObjectName("layoutPreview"); preview->setDragMode(QGraphicsView::ScrollHandDrag); preview->setRenderHint(QPainter::SmoothPixmapTransform);
    preview->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); preview->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff); preview->viewport()->installEventFilter(this); pageLayout->addWidget(preview,1);
    connect(fit,&QToolButton::clicked,this,[this]{previewTarget=image.rect(); fitPreview();});
    connect(in,&QToolButton::clicked,this,[this]{previewTarget={}; preview->scale(1.25,1.25);});
    connect(out,&QToolButton::clicked,this,[this]{previewTarget={}; preview->scale(.8,.8);});
    for (const auto &region:existing) {
        auto *box=scene->addRect(region.bounds,QPen(QColor(128,138,151),2,Qt::DashLine));
        box->setToolTip(region.autoDetected ? tr("Existing automatic block") : tr("Preserved manual block")); box->setZValue(1); preservedBoxes.append(box);
    }
    auto *blockPanel=new QWidget(splitter); blockPanel->setMinimumWidth(400); auto *blockLayout=new QVBoxLayout(blockPanel); blockLayout->setContentsMargins(8,0,0,0); blockLayout->setSpacing(8);
    auto *blockHeading=new QHBoxLayout; auto *blocksTitle=new QLabel(tr("BLOCKS"),blockPanel); blocksTitle->setObjectName("layoutPanelTitle"); blockHeading->addWidget(blocksTitle);
    blockCount=new QLabel(blockPanel); blockCount->setObjectName("layoutCount"); blockHeading->addWidget(blockCount); blockHeading->addStretch(); blockLayout->addLayout(blockHeading);
    emptyHint=new QLabel(tr("Finding areas on this page…\nBlock miniatures will appear here."),blockPanel); emptyHint->setObjectName("layoutSubtitle"); emptyHint->setAlignment(Qt::AlignCenter); emptyHint->setWordWrap(true); blockLayout->addWidget(emptyHint);
    miniatures=new QListWidget(blockPanel); miniatures->setObjectName("layoutAreas"); miniatures->setSpacing(5); miniatures->setSelectionMode(QAbstractItemView::SingleSelection); miniatures->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); miniatures->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel); blockLayout->addWidget(miniatures,1);
    miniatures->setItemDelegate(new BlockCardDelegate(miniatures));
    splitter->setSizes({660,440}); layout->addWidget(splitter,1);
    progress=new QProgressBar(this); progress->setRange(0,0); progress->hide(); layout->addWidget(progress);
    message=new QLabel(tr("Loading the current page and its blocks…"),this); message->setTextFormat(Qt::PlainText); message->setObjectName("layoutMessage"); message->setWordWrap(true); layout->addWidget(message);
    auto *footer=new QHBoxLayout;
    analyzeButton=new QPushButton(aiPageAreasIcon(),tr("Analyze again"),this); analyzeButton->setObjectName("analyzePageAreas"); footer->addWidget(analyzeButton); footer->addStretch();
    auto *buttons=new QDialogButtonBox(QDialogButtonBox::Cancel,this);
    applyButton=buttons->addButton(tr("Apply areas"),QDialogButtonBox::AcceptRole); applyButton->setObjectName("applyPageAreas"); applyButton->setEnabled(false); footer->addWidget(buttons); layout->addLayout(footer);
    applyButton->setStyleSheet(QStringLiteral("QPushButton {background: #216ec1; color: white; border: 1px solid #216ec1; padding: 8px 18px; font-weight: 700;} QPushButton:hover {background: #145fae;} QPushButton:disabled {background: #e7edf4; color: #a1acba; border-color: #e1e6ed;}"));
    connect(buttons,&QDialogButtonBox::rejected,this,&AiPageLayoutDialog::reject); connect(applyButton,&QPushButton::clicked,this,&QDialog::accept); connect(analyzeButton,&QPushButton::clicked,this,&AiPageLayoutDialog::analyze);
    connect(replaceAutomatic,&QCheckBox::toggled,this,[this]{populate(false);});
    connect(miniatures,&QListWidget::itemChanged,this,[this](QListWidgetItem *item){if (!updating && item->data(Qt::UserRole).toInt()>=0) updateSelection(item->data(Qt::UserRole).toInt());});
    connect(miniatures,&QListWidget::currentItemChanged,this,[this](QListWidgetItem *item){if (!updating) {updateSelection(); focusBlock(item);}});
    connect(&watcher,&QFutureWatcherBase::finished,this,[this]{
        busy=false; progress->hide(); analyzeButton->setEnabled(true); endpoint->setEnabled(true); token->setEnabled(true); threshold->setEnabled(true); replaceAutomatic->setEnabled(true);
        auto result=watcher.result();
        if (!result) {
            message->setText(result.error().cancelled ? tr("Analysis cancelled.") : result.error().message);
            emptyHint->setText(tr("Analysis unavailable.\nYour current page is unchanged.")); updateSelection(); return;
        }
        areas=std::move(result->areas); hasAnalysis=true; populate();
        message->setText(areas.isEmpty() ? tr("No areas found. Open Options to lower the minimum score, then analyze again.") : tr("Select a miniature to inspect a block. “Also likely” offers another strong model classification. Manual blocks stay in place."));
    });
    populate();
}
QString AiPageLayoutDialog::sessionToken() const {return token->text();}
AiPageLayoutDialog::~AiPageLayoutDialog() {stop.request_stop(); watcher.waitForFinished();}
void AiPageLayoutDialog::reject() {stop.request_stop(); QDialog::reject();}
void AiPageLayoutDialog::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
    if (!opened) {opened=true; QTimer::singleShot(0,this,[this]{fitPreview(); analyze();});}
}
bool AiPageLayoutDialog::eventFilter(QObject *object,QEvent *event)
{
    if (object==preview->viewport() && event->type()==QEvent::Resize) QTimer::singleShot(0,this,[this]{fitPreview();});
    return QDialog::eventFilter(object,event);
}
void AiPageLayoutDialog::fitPreview() {if (!previewTarget.isEmpty()) preview->fitInView(previewTarget,Qt::KeepAspectRatio);}
void AiPageLayoutDialog::focusBlock(QListWidgetItem *item)
{
    if (!item) return;
    const int index=item->data(Qt::UserRole).toInt();
    const QRect bounds=index>=0 ? areas[index].bounds : existing[-index-1].bounds;
    const int margin=std::max(12,std::min(bounds.width(),bounds.height())/8);
    previewTarget=bounds.adjusted(-margin,-margin,margin,margin).intersected(image.rect()); fitPreview();
}
void AiPageLayoutDialog::analyze()
{
    if (busy) return;
    processing::PageLayoutOptions options{QUrl(endpoint->text().trimmed()),token->text(),threshold->value()};
    QSettings settings("YAGF","YAGF Revival"); settings.setValue("layout/url",options.url.toString()); settings.setValue("layout/threshold",options.threshold);
    stop=std::stop_source{}; busy=true; areas.clear(); hasAnalysis=false; populate();
    analyzeButton->setEnabled(false); endpoint->setEnabled(false); token->setEnabled(false); threshold->setEnabled(false); replaceAutomatic->setEnabled(false);
    progress->show(); message->setText(tr("Finding text, pictures and formulas on the current page…"));
    watcher.setFuture(QtConcurrent::run([source=image,options,token=stop.get_token()]{return processing::detectPageAreas(source,options,token);}));
}
void AiPageLayoutDialog::populate(bool resetSelection)
{
    QSet<int> previouslySelected;
    if (!resetSelection) for (int i=0;i<areaItems.size();++i) if (areaItems[i]->checkState()==Qt::Checked) previouslySelected.insert(i);
    updating=true;
    for (auto *box:boxes) delete box;
    boxes.clear(); cards.clear(); areaItems.clear(); miniatures->clear();
    for (int i=0;i<existing.size();++i) {
        const auto &region=existing[i]; const bool kept=!replaceAutomatic->isChecked() || !region.autoDetected;
        preservedBoxes[i]->setVisible(kept || !hasAnalysis);
        if (!kept && hasAnalysis) continue;
        auto *item=new QListWidgetItem(miniatures); item->setData(Qt::UserRole,-i-1); item->setSizeHint(QSize(380,144));
        auto *card=new QFrame(miniatures); card->setStyleSheet("QFrame {background: #f0f2f5; border-radius: 8px;}"); auto *row=new QHBoxLayout(card); row->setContentsMargins(10,10,10,10);
        auto *thumb=thumbnail(image,region.bounds,card); row->addWidget(thumb); auto *details=new QVBoxLayout;
        auto *title=new QLabel(region.autoDetected ? tr("Existing block") : tr("Manual block"),card); title->setObjectName("layoutPanelTitle"); details->addWidget(title);
        auto *type=new QLabel(region.notText && region.content!="picture" ? tr("Excluded from OCR") : typeLabel(region.content),card); details->addWidget(type);
        auto *state=new QLabel(kept ? tr("Kept on the page") : tr("Will be replaced after Apply"),card); state->setWordWrap(true); state->setObjectName("layoutSubtitle"); details->addWidget(state); details->addStretch(); row->addLayout(details,1);
        miniatures->setItemWidget(item,card); connect(thumb,&QToolButton::clicked,this,[this,item]{miniatures->setCurrentItem(item); focusBlock(item);});
    }
    for (int i=0;i<areas.size();++i) {
        if (areas[i].detectedType.isEmpty()) areas[i].detectedType=areas[i].type;
        auto *item=new QListWidgetItem(miniatures); item->setFlags(Qt::ItemIsEnabled|Qt::ItemIsSelectable|Qt::ItemIsUserCheckable); item->setData(Qt::UserRole,i); item->setCheckState(Qt::Unchecked); item->setSizeHint(QSize(380,158)); areaItems.append(item);
        auto *card=new QFrame(miniatures); card->setObjectName("layoutBlockCard"); auto *row=new QHBoxLayout(card); row->setContentsMargins(10,10,10,10); row->setSpacing(12);
        auto *thumb=thumbnail(image,areas[i].bounds,card); row->addWidget(thumb);
        auto *details=new QVBoxLayout; details->setSpacing(5); auto *use=new QCheckBox(tr("Block %1").arg(i+1),card); use->setObjectName(QString("layoutUseArea%1").arg(i)); details->addWidget(use);
        auto *type=new AreaTypeCombo(card); type->setObjectName(QString("layoutType%1").arg(i)); type->addItem(tr("Text"),"text"); type->addItem(tr("Picture"),"picture"); type->addItem(tr("Formula"),"math"); type->addItem(tr("Text + formulas"),"mixed"); type->setCurrentIndex(type->findData(areas[i].type)); details->addWidget(type);
        auto *score=new QLabel(card); score->setObjectName("layoutBlockScore"); score->setTextFormat(Qt::PlainText); score->setWordWrap(true); details->addWidget(score);
        auto *alternative=new QToolButton(card); alternative->setObjectName("layoutAlternative"); alternative->setSizePolicy(QSizePolicy::Preferred,QSizePolicy::Fixed); details->addWidget(alternative); details->addStretch(); row->addLayout(details,1);
        cards.append({card,use,type,score,alternative}); miniatures->setItemWidget(item,card);
        connect(use,&QCheckBox::clicked,this,[this,i,item](bool checked){miniatures->setCurrentItem(item); item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);});
        connect(type,&QComboBox::currentIndexChanged,this,[this,i,type,item]{areas[i].type=type->currentData().toString(); miniatures->setCurrentItem(item); updateSelection();});
        connect(alternative,&QToolButton::clicked,this,[this,i,alternative]{cards[i].type->setCurrentIndex(cards[i].type->findData(alternative->property("alternativeType").toString()));});
        connect(thumb,&QToolButton::clicked,this,[this,item]{miniatures->setCurrentItem(item); focusBlock(item);});
        auto *box=new AreaOutline(areas[i].bounds,[this,item]{miniatures->setCurrentItem(item); miniatures->scrollToItem(item); focusBlock(item);}); scene->addItem(box); box->setZValue(2); boxes.append(box);
    }
    QVector<QRect> occupied;
    for (const auto &block:existing) if (!replaceAutomatic->isChecked() || !block.autoDetected) occupied.append(block.bounds);
    QVector<int> order(areas.size()); std::iota(order.begin(),order.end(),0);
    std::stable_sort(order.begin(),order.end(),[&](int a,int b){return areas[a].score>areas[b].score;});
    for (int i:order) {
        const bool overlap=std::ranges::any_of(occupied,[&](const auto &box){return box.intersects(areas[i].bounds);});
        const bool preservedOverlap=std::ranges::any_of(existing,[&](const auto &block){return (!replaceAutomatic->isChecked() || !block.autoDetected) && block.bounds.intersects(areas[i].bounds);});
        if (preservedOverlap) {
            areaItems[i]->setFlags(Qt::ItemIsSelectable|Qt::ItemIsEnabled);
            areaItems[i]->setToolTip(tr("Overlaps a kept block. Adjust that block on the page before analyzing again.")); cards[i].use->setEnabled(false); cards[i].use->setToolTip(areaItems[i]->toolTip());
        } else if (!overlap && (resetSelection || previouslySelected.contains(i))) {areaItems[i]->setCheckState(Qt::Checked); occupied.append(areas[i].bounds);}
    }
    updating=false; updateSelection();
    emptyHint->setVisible(miniatures->count()==0); emptyHint->setText(hasAnalysis ? tr("No areas found.\nTry a lower minimum score in Options.") : tr("Finding areas on this page…\nBlock miniatures will appear here."));
}
void AiPageLayoutDialog::updateCard(int index)
{
    const auto &area=areas[index]; const auto &card=cards[index];
    QVector<processing::PageAreaAlternative> candidates{{area.label,area.detectedType,area.score}}; candidates+=area.alternatives;
    const auto chosen=std::ranges::find(candidates,area.type,&processing::PageAreaAlternative::type);
    const QString caption=chosen==candidates.end() ? tr("Manual type · model: %1").arg(typeLabel(area.detectedType)) : tr("Model score %1% · %2").arg(chosen->score*100,0,'f',1).arg(QString(chosen->label).replace('_',' '));
    card.score->setText(caption); card.score->setToolTip(caption);
    const auto other=std::ranges::find_if(candidates,[&](const auto &candidate){return candidate.type!=area.type;});
    const bool hasAlternative=!area.alternatives.isEmpty() && other!=candidates.end();
    card.alternative->setVisible(hasAlternative);
    if (hasAlternative) {
        card.alternative->setText(tr("Also likely: %1\n%2% model score").arg(typeLabel(other->type)).arg(other->score*100,0,'f',0));
        card.alternative->setProperty("alternativeType",other->type);
        card.alternative->setToolTip(tr("Switch this block to %1 (%2). This score comes from the same detected region.").arg(typeLabel(other->type),other->label));
    }
    const bool active=miniatures->currentItem()==areaItems[index];
    card.frame->setStyleSheet(QString("QFrame#layoutBlockCard {background: %1; border: %2px solid %3; border-radius: 9px;}")
        .arg(active ? "#f4f8ff" : "#ffffff").arg(active ? 2 : 1).arg(active ? areaColor(area.type).name() : "#dfe5ed"));
    const QSignalBlocker blocker(card.use); card.use->setChecked(areaItems[index]->checkState()==Qt::Checked);
}
void AiPageLayoutDialog::updateSelection(int changed)
{
    if (updating) return;
    updating=true;
    if (changed>=0 && areaItems[changed]->checkState()==Qt::Checked) {
        for (int i=0;i<areas.size();++i) if (i!=changed && areas[i].bounds.intersects(areas[changed].bounds)) areaItems[i]->setCheckState(Qt::Unchecked);
    }
    int selected=0;
    for (int i=0;i<areas.size();++i) {
        const bool checked=areaItems[i]->checkState()==Qt::Checked; selected+=checked;
        QColor color=checked ? areaColor(areas[i].type) : QColor(143,152,164);
        const bool active=miniatures->currentItem()==areaItems[i];
        QPen pen(color,active ? 2.5 : 1.5,checked ? Qt::SolidLine : Qt::DashLine); pen.setCosmetic(true); boxes[i]->setPen(pen);
        color.setAlpha(checked ? 22 : 5); boxes[i]->setBrush(color); boxes[i]->setToolTip(tr("Block %1 · %2").arg(i+1).arg(typeLabel(areas[i].type))); updateCard(i);
    }
    blockCount->setText(hasAnalysis ? tr("%1 selected / %2 proposed").arg(selected).arg(areas.size()) : tr("%1 existing").arg(existing.size()));
    applyButton->setEnabled(!busy && hasAnalysis && selected>0 && regions().size()<=1000);
    updating=false;
}
QVector<core::ImageRegion> AiPageLayoutDialog::regions() const
{
    QVector<processing::PageArea> accepted;
    for (int i=0;i<areas.size();++i) if (areaItems[i]->checkState()==Qt::Checked) accepted.append(areas[i]);
    return processing::mergePageAreas(existing,accepted,replaceAutomatic->isChecked());
}
}

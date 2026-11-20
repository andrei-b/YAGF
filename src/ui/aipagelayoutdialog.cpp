#include "aipagelayoutdialog.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSplitter>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <algorithm>
#include <numeric>

namespace yagf::ui {
namespace {
QColor areaColor(const QString &type) {
    return type=="picture" ? QColor(174,109,46) : type=="math" ? QColor(49,93,172) :
           type=="mixed" ? QColor(127,72,153) : QColor(25,135,55);
}
}
AiPageLayoutDialog::AiPageLayoutDialog(QImage source, QVector<core::ImageRegion> blocks, QWidget *parent)
    :QDialog(parent),image(std::move(source)),existing(std::move(blocks))
{
    setWindowTitle(tr("AI page areas")); setObjectName("aiPageLayoutDialog"); resize(1100,780);
    auto *layout=new QVBoxLayout(this);
    auto *intro=new QLabel(tr("Detect text, pictures and formulas, then review the proposed blocks. Manually adjusted blocks are always preserved. Pictures are excluded from OCR."),this);
    intro->setWordWrap(true); layout->addWidget(intro);
    auto *form=new QFormLayout;
    QSettings settings;
    endpoint=new QLineEdit(settings.value("layout/url","http://127.0.0.1:8771").toString(),this); endpoint->setObjectName("layoutEndpoint");
    endpoint->setToolTip(tr("Layout worker URL. The configured Jetson tunnel uses port 8771."));
    token=new QLineEdit(this); token->setEchoMode(QLineEdit::Password); token->setPlaceholderText(tr("Optional; kept only for this dialog"));
    threshold=new QDoubleSpinBox(this); threshold->setObjectName("layoutThreshold"); threshold->setRange(.1,.95); threshold->setSingleStep(.05);
    threshold->setValue(settings.value("layout/threshold",.5).toDouble());
    form->addRow(tr("Layout service:"),endpoint); form->addRow(tr("Bearer token:"),token); form->addRow(tr("Minimum model score:"),threshold); layout->addLayout(form);
    replaceAutomatic=new QCheckBox(tr("Replace automatically detected blocks"),this); replaceAutomatic->setChecked(true);
    layout->addWidget(replaceAutomatic);
    auto *splitter=new QSplitter(this);
    scene=new QGraphicsScene(this); scene->addPixmap(QPixmap::fromImage(image)); scene->setSceneRect(image.rect());
    preview=new QGraphicsView(scene,splitter); preview->setObjectName("layoutPreview"); preview->setDragMode(QGraphicsView::ScrollHandDrag);
    for (const auto &region:existing) {
        auto *box=scene->addRect(region.bounds,QPen(QColor(90,90,90),3,Qt::DashLine));
        box->setToolTip(region.autoDetected ? tr("Preserved automatic block") : tr("Preserved manual block")); box->setZValue(1);
        box->setVisible(!region.autoDetected); preservedBoxes.append(box);
    }
    table=new QTableWidget(0,4,splitter); table->setObjectName("layoutAreas");
    table->setHorizontalHeaderLabels({tr("Use"),tr("Content"),tr("Detected label"),tr("Model score")});
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table->horizontalHeader()->setStretchLastSection(true); table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers); splitter->setSizes({600,430}); layout->addWidget(splitter,1);
    message=new QLabel(tr("Click Analyze to propose page areas. The page stays unchanged until Apply."),this);
    message->setTextFormat(Qt::PlainText);
    message->setObjectName("layoutMessage"); message->setWordWrap(true); layout->addWidget(message);
    progress=new QProgressBar(this); progress->setRange(0,0); progress->hide(); layout->addWidget(progress);
    auto *buttons=new QDialogButtonBox(QDialogButtonBox::Cancel,this);
    analyzeButton=buttons->addButton(tr("Analyze"),QDialogButtonBox::ActionRole); analyzeButton->setObjectName("analyzePageAreas");
    applyButton=buttons->addButton(tr("Apply"),QDialogButtonBox::AcceptRole); applyButton->setObjectName("applyPageAreas"); applyButton->setEnabled(false);
    layout->addWidget(buttons);
    connect(buttons,&QDialogButtonBox::rejected,this,&AiPageLayoutDialog::reject);
    connect(applyButton,&QPushButton::clicked,this,&QDialog::accept);
    connect(analyzeButton,&QPushButton::clicked,this,&AiPageLayoutDialog::analyze);
    connect(replaceAutomatic,&QCheckBox::toggled,this,[this]{ populate(); });
    connect(table,&QTableWidget::itemChanged,this,[this](QTableWidgetItem *item){ if (!updating && item->column()==0) updateSelection(item->row()); });
    connect(table,&QTableWidget::currentCellChanged,this,[this](int,int,int,int){ updateSelection(); });
    connect(&watcher,&QFutureWatcherBase::finished,this,[this]{
        busy=false; progress->hide(); analyzeButton->setEnabled(true); endpoint->setEnabled(true); token->setEnabled(true); threshold->setEnabled(true); replaceAutomatic->setEnabled(true);
        auto result=watcher.result();
        if (!result) { message->setText(result.error().cancelled ? tr("Analysis cancelled.") : result.error().message); return; }
        areas=std::move(result->areas); populate();
        message->setText(tr("%1 proposed %2 areas. Check their labels and selection before applying. Gray outlines are preserved manual blocks. Overlapping proposals are initially unchecked.").arg(result->model).arg(areas.size()));
    });
}
AiPageLayoutDialog::~AiPageLayoutDialog() { stop.request_stop(); watcher.waitForFinished(); }
void AiPageLayoutDialog::reject() { stop.request_stop(); QDialog::reject(); }
void AiPageLayoutDialog::resizeEvent(QResizeEvent *event) { QDialog::resizeEvent(event); preview->fitInView(scene->sceneRect(),Qt::KeepAspectRatio); }
void AiPageLayoutDialog::analyze()
{
    if (busy) return;
    processing::PageLayoutOptions options{QUrl(endpoint->text().trimmed()),token->text(),threshold->value()};
    QSettings settings; settings.setValue("layout/url",options.url.toString()); settings.setValue("layout/threshold",options.threshold);
    stop=std::stop_source{}; busy=true; areas.clear(); populate();
    applyButton->setEnabled(false); analyzeButton->setEnabled(false); endpoint->setEnabled(false); token->setEnabled(false); threshold->setEnabled(false); replaceAutomatic->setEnabled(false);
    progress->show(); message->setText(tr("Analyzing page areas on the layout worker…"));
    watcher.setFuture(QtConcurrent::run([source=image,options,token=stop.get_token()]{ return processing::detectPageAreas(source,options,token); }));
}
void AiPageLayoutDialog::populate()
{
    updating=true;
    for (int i=0;i<existing.size();++i) preservedBoxes[i]->setVisible(!replaceAutomatic->isChecked() || !existing[i].autoDetected);
    for (auto *box:boxes) delete box;
    boxes.clear(); table->setRowCount(0); table->setRowCount(areas.size());
    for (int i=0;i<areas.size();++i) {
        auto *use=new QTableWidgetItem; use->setFlags(Qt::ItemIsEnabled|Qt::ItemIsUserCheckable|Qt::ItemIsSelectable); use->setCheckState(Qt::Unchecked);
        table->setItem(i,0,use);
        auto *type=new QComboBox(table); type->addItem(tr("Text"),"text"); type->addItem(tr("Picture"),"picture"); type->addItem(tr("Formula"),"math"); type->addItem(tr("Text + formulas"),"mixed"); type->setCurrentIndex(type->findData(areas[i].type)); table->setCellWidget(i,1,type);
        connect(type,&QComboBox::currentIndexChanged,this,[this,i,type]{ areas[i].type=type->currentData().toString(); updateSelection(); });
        table->setItem(i,2,new QTableWidgetItem(areas[i].label)); table->setItem(i,3,new QTableWidgetItem(QString::number(areas[i].score*100,'f',1)+"%"));
        auto *box=scene->addRect(areas[i].bounds); box->setZValue(2); boxes.append(box);
    }
    QVector<QRect> occupied;
    for (const auto &block:existing) if (!replaceAutomatic->isChecked() || !block.autoDetected) occupied.append(block.bounds);
    QVector<int> order(areas.size()); std::iota(order.begin(),order.end(),0);
    std::stable_sort(order.begin(),order.end(),[&](int a,int b){ return areas[a].score>areas[b].score; });
    for (int i:order) {
        bool preservedOverlap=false;
        for (const auto &block:existing) if ((!replaceAutomatic->isChecked() || !block.autoDetected) && block.bounds.intersects(areas[i].bounds)) preservedOverlap=true;
        if (preservedOverlap) {
            table->item(i,0)->setFlags(Qt::ItemIsSelectable);
            table->item(i,0)->setToolTip(tr("Overlaps a preserved block. Adjust or remove that block on the page before analyzing again."));
        } else if (std::ranges::none_of(occupied,[&](const auto &box){return box.intersects(areas[i].bounds);})) {
            table->item(i,0)->setCheckState(Qt::Checked); occupied.append(areas[i].bounds);
        }
    }
    updating=false; updateSelection();
    preview->fitInView(scene->sceneRect(),Qt::KeepAspectRatio);
}
void AiPageLayoutDialog::updateSelection(int changed)
{
    if (updating) return;
    updating=true;
    if (changed>=0 && table->item(changed,0)->checkState()==Qt::Checked) {
        for (int i=0;i<areas.size();++i) if (i!=changed && areas[i].bounds.intersects(areas[changed].bounds)) table->item(i,0)->setCheckState(Qt::Unchecked);
    }
    int selected=0;
    for (int i=0;i<areas.size();++i) {
        const bool checked=table->item(i,0)->checkState()==Qt::Checked; selected+=checked;
        QColor color=areaColor(areas[i].type); if (!checked) color=QColor(125,125,125);
        boxes[i]->setPen(QPen(color,table->currentRow()==i ? 5 : 2,checked ? Qt::SolidLine : Qt::DashLine));
        color.setAlpha(checked ? 35 : 8); boxes[i]->setBrush(color);
        boxes[i]->setToolTip(tr("%1 · %2").arg(i+1).arg(areas[i].label));
    }
    const auto merged=regions();
    applyButton->setEnabled(!busy && selected>0 && merged.size()<=1000);
    updating=false;
}
QVector<core::ImageRegion> AiPageLayoutDialog::regions() const
{
    QVector<processing::PageArea> accepted;
    for (int i=0;i<areas.size();++i) if (table->item(i,0)->checkState()==Qt::Checked) accepted.append(areas[i]);
    return processing::mergePageAreas(existing,accepted,replaceAutomatic->isChecked());
}
}

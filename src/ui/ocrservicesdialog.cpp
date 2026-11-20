#include "ocrservicesdialog.h"
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <algorithm>

namespace yagf::ui {
using namespace processing;
OcrServicesDialog::OcrServicesDialog(QVector<OcrService> services,OcrService active,QWidget *parent)
    :QDialog(parent),entries(std::move(services)),primary(std::move(active)) {
    setObjectName("ocrServicesDialog"); setWindowTitle(tr("OCR services and parallel recognition")); resize(1180,650);
    auto *layout=new QVBoxLayout(this);
    auto *intro=new QLabel(tr("Discover OCR services on named hosts or forwarded ports. Keep one primary reading and run up to three companion engines at the same time. Review their alternatives after recognition."),this); intro->setWordWrap(true); layout->addWidget(intro);
    auto *search=new QHBoxLayout;
    hosts=new QLineEdit(QSettings("YAGF","YAGF Revival").value("ocr/discoveryHosts","127.0.0.1").toString(),this); hosts->setObjectName("ocrDiscoveryHosts");
    hosts->setPlaceholderText(tr("127.0.0.1, jetson-host, https://server:8765")); hosts->setToolTip(tr("Host names probe ports 8765–8769 and 8772. A URL or host:port probes that exact service. Existing configured URLs are also checked. Up to 32 endpoints; no document images are sent during discovery."));
    search->addWidget(new QLabel(tr("Hosts / service URLs:"),this)); search->addWidget(hosts,1);
    scan=new QPushButton(tr("Discover"),this); scan->setObjectName("discoverOcrServices"); search->addWidget(scan);
    stopButton=new QPushButton(tr("Stop"),this); search->addWidget(stopButton); layout->addLayout(search);
    table=new QTableWidget(0,6,this); table->setObjectName("ocrServicesTable");
    table->setHorizontalHeaderLabels({tr("Use together"),tr("Name"),tr("Engine"),tr("Service URL"),tr("Language"),tr("Status")});
    table->setSelectionBehavior(QAbstractItemView::SelectRows); table->setSelectionMode(QAbstractItemView::SingleSelection); table->verticalHeader()->hide();
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive); table->horizontalHeader()->setSectionResizeMode(3,QHeaderView::Stretch);
    for (const auto &[column,width]:{std::pair{0,95},{1,195},{2,115},{4,90},{5,145}}) table->setColumnWidth(column,width);
    layout->addWidget(table,1);
    auto *tools=new QHBoxLayout; add=new QPushButton(tr("Add service…"),this); remove=new QPushButton(tr("Remove"),this); usePrimary=new QPushButton(tr("Use selected as primary"),this); usePrimary->setObjectName("usePrimaryOcrService");
    tools->addWidget(add); tools->addWidget(remove); tools->addStretch(); tools->addWidget(usePrimary); layout->addLayout(tools);
    auto *auth=new QHBoxLayout; auth->addWidget(new QLabel(tr("Selected service token:"),this)); token=new QLineEdit(this); token->setObjectName("ocrServiceToken"); token->setEchoMode(QLineEdit::Password); token->setPlaceholderText(tr("Optional bearer token; kept only in memory")); auth->addWidget(token,1); layout->addLayout(auth);
    summary=new QLabel(this); summary->setTextFormat(Qt::PlainText); summary->setWordWrap(true); layout->addWidget(summary);
    status=new QLabel(this); status->setObjectName("ocrDiscoveryStatus"); status->setTextFormat(Qt::PlainText); status->setWordWrap(true); layout->addWidget(status);
    auto *buttons=new QDialogButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Cancel,this); saveButton=buttons->button(QDialogButtonBox::Save); layout->addWidget(buttons);
    if (std::ranges::none_of(entries,[](const auto &s){return s.provider=="tesseract";})) {
        OcrService local; local.provider="tesseract"; local.label=ocrProviderLabel(local.provider); local.ready=!QStandardPaths::findExecutable("tesseract").isEmpty(); entries.prepend(local);
    }
    if (ocrProviders().contains(primary.provider) && std::ranges::none_of(entries,[&](const auto &s){return s.key()==primary.key();})) entries.append(primary);
    connect(table,&QTableWidget::cellChanged,this,[this](int row,int column) {
        if (filling || row<0 || row>=entries.size()) return;
        auto &s=entries[row]; const auto previous=s.key();
        if (column==0) s.enabled=table->item(row,0)->checkState()==Qt::Checked;
        else if (column==1) s.label=table->item(row,1)->text().trimmed();
        else if (column==3) {s.url=QUrl(table->item(row,3)->text().trimmed()); s.token.clear(); s.ready=false;}
        else if (column==4) s.language=table->item(row,4)->text().trimmed();
        if (selectedPrimary==previous) selectedPrimary=s.key(); refresh();
    });
    connect(table,&QTableWidget::currentCellChanged,this,[this] {const int row=table->currentRow(); const QSignalBlocker block(token); token->setText(row>=0 && row<entries.size() ? entries[row].token : QString()); refresh();});
    connect(token,&QLineEdit::textEdited,this,[this](const QString &value) {const int row=table->currentRow(); if (row>=0) entries[row].token=value;});
    connect(scan,&QPushButton::clicked,this,&OcrServicesDialog::discover); connect(stopButton,&QPushButton::clicked,this,[this]{stop.request_stop();});
    connect(usePrimary,&QPushButton::clicked,this,&OcrServicesDialog::choosePrimary); connect(add,&QPushButton::clicked,this,&OcrServicesDialog::addService);
    connect(remove,&QPushButton::clicked,this,[this] {const int row=table->currentRow(); if (row<0 || entries[row].provider=="tesseract") return; if (entries[row].key()==selectedPrimary) selectedPrimary.clear(); entries.removeAt(row); populate();});
    connect(buttons,&QDialogButtonBox::accepted,this,&OcrServicesDialog::save); connect(buttons,&QDialogButtonBox::rejected,this,&OcrServicesDialog::reject);
    connect(&watcher,&QFutureWatcherBase::finished,this,[this] {
        busy=false; auto result=watcher.result();
        if (!result) status->setText(result.error().cancelled ? tr("Discovery stopped.") : result.error().message);
        else {
            for (auto &s:entries) if (s.provider!="tesseract") {s.ready=false; s.error=tr("No response in this discovery");}
            for (auto discovered:result->services) {
                auto found=std::ranges::find_if(entries,[&](const auto &s){return s.key()==discovered.key();});
                if (found==entries.end()) {if (entries.size()<64) entries.append(std::move(discovered));}
                else {found->ready=discovered.ready; found->error=discovered.error; if (found->language.isEmpty()) found->language=discovered.language;}
            }
            const int ready=std::ranges::count_if(result->services,[](const auto &s){return s.ready;});
            status->setText(tr("%1 remote OCR services ready. Unavailable endpoints can be checked again after starting their service or SSH tunnel.").arg(ready));
            status->setToolTip(result->diagnostics.join('\n'));
        }
        populate();
    });
    populate();
}
OcrServicesDialog::~OcrServicesDialog() {stop.request_stop(); watcher.waitForFinished();}
void OcrServicesDialog::reject() {stop.request_stop(); QDialog::reject();}
void OcrServicesDialog::showEvent(QShowEvent *event) {QDialog::showEvent(event); if (!opened) {opened=true; QTimer::singleShot(0,this,&OcrServicesDialog::discover);}}
void OcrServicesDialog::populate(int selected) {
    filling=true; table->setRowCount(entries.size());
    for (int row=0;row<entries.size();++row) {
        const auto &s=entries[row];
        auto *check=new QTableWidgetItem; check->setFlags(Qt::ItemIsEnabled|Qt::ItemIsSelectable|Qt::ItemIsUserCheckable); check->setCheckState(s.enabled ? Qt::Checked : Qt::Unchecked); table->setItem(row,0,check);
        const QStringList values{s.label,ocrProviderLabel(s.provider),s.url.toString(),s.language,s.ready ? tr("Ready") : s.error.isEmpty() ? tr("Not checked") : s.error};
        for (int column=1;column<6;++column) {
            auto *item=new QTableWidgetItem(values[column-1]); item->setToolTip(values[column-1]);
            if (column==2 || column==5 || (column==3 && s.provider=="tesseract")) item->setFlags(item->flags()&~Qt::ItemIsEditable);
            if (column==4) item->setToolTip(tr("An empty local Tesseract language follows the primary language. Remote services normally report their fixed language or auto."));
            table->setItem(row,column,item);
        }
        table->setRowHeight(row,42);
    }
    table->setCurrentCell(entries.isEmpty() ? -1 : std::clamp(selected,0,int(entries.size())-1),1); filling=false; refresh();
}
void OcrServicesDialog::refresh() {
    const int row=table->currentRow(); const bool selected=row>=0 && row<entries.size();
    const auto active=primaryService(); const auto key=active ? active->key() : primary.key();
    const int enabled=std::ranges::count_if(entries,[&](const auto &s){return s.enabled && s.key()!=key;});
    summary->setText(tr("Primary: %1 · %2 companion engines selected. Discovery sends health checks only; recognition sends the selected page/blocks to the enabled engines.").arg(active ? active->label : primary.label).arg(enabled));
    table->setEnabled(!busy); hosts->setEnabled(!busy); scan->setEnabled(!busy); stopButton->setVisible(busy); add->setEnabled(!busy && entries.size()<64);
    remove->setEnabled(!busy && selected && entries[row].provider!="tesseract"); usePrimary->setEnabled(!busy && selected); token->setEnabled(!busy && selected && entries[row].provider!="tesseract"); saveButton->setEnabled(!busy && enabled<=3);
}
void OcrServicesDialog::discover() {
    if (busy) return; busy=true; stop=std::stop_source{}; const auto cancellation=stop.get_token();
    const auto targets=ocrDiscoveryTargets(hosts->text(),entries);
    const auto known=entries;
    status->setText(tr("Checking OCR service health…")); refresh();
    watcher.setFuture(QtConcurrent::run([targets,known,cancellation]{return discoverOcrServices(targets,known,cancellation);}));
}
void OcrServicesDialog::choosePrimary() {
    const int row=table->currentRow(); if (row<0) return; selectedPrimary=entries[row].key(); entries[row].enabled=false; populate(row);
}
std::optional<OcrService> OcrServicesDialog::primaryService() const {
    if (selectedPrimary.isEmpty()) return {};
    const auto found=std::ranges::find_if(entries,[&](const auto &s){return s.key()==selectedPrimary;});
    return found==entries.end() ? std::optional<OcrService>{} : *found;
}
void OcrServicesDialog::save() {
    for (const auto &s:entries) if (s.label.isEmpty() || s.label.size()>128 || s.language.size()>64 || (s.provider!="tesseract" && !validOcrServiceUrl(s.url))) {
        status->setText(tr("Enter a name, a valid HTTP(S) service URL and a language of up to 64 characters for each service.")); return;
    }
    for (const auto &host:hosts->text().split(QRegularExpression("[\\s,;]+"),Qt::SkipEmptyParts))
        if (!validOcrServiceUrl(QUrl(host.contains("://") ? host : "http://"+host))) {
            status->setText(tr("Enter host names or HTTP(S) URLs without embedded credentials, query or fragment.")); return;
        }
    QSettings("YAGF","YAGF Revival").setValue("ocr/discoveryHosts",hosts->text().trimmed()); accept();
}
void OcrServicesDialog::addService() {
    QDialog dialog(this); dialog.setWindowTitle(tr("Add OCR service")); auto *form=new QFormLayout(&dialog);
    auto *provider=new QComboBox(&dialog); for (const auto &p:ocrProviders()) if (p!="tesseract") provider->addItem(ocrProviderLabel(p),p);
    auto *name=new QLineEdit(&dialog),*url=new QLineEdit("http://127.0.0.1:8765",&dialog),*language=new QLineEdit("auto",&dialog); name->setMaxLength(128); language->setMaxLength(64);
    form->addRow(tr("Engine:"),provider); form->addRow(tr("Name:"),name); form->addRow(tr("Service URL:"),url); form->addRow(tr("Language:"),language);
    auto *error=new QLabel(&dialog); error->setWordWrap(true); form->addRow(error);
    auto *buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&dialog); form->addRow(buttons);
    connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    connect(buttons,&QDialogButtonBox::accepted,&dialog,[&] {if (!validOcrServiceUrl(QUrl(url->text().trimmed()))) error->setText(tr("Enter an HTTP(S) URL without credentials, query or fragment.")); else dialog.accept();});
    if (dialog.exec()!=QDialog::Accepted) return;
    OcrService service; service.provider=provider->currentData().toString(); service.label=name->text().trimmed(); if (service.label.isEmpty()) service.label=ocrProviderLabel(service.provider); service.url=QUrl(url->text().trimmed()); service.language=language->text().trimmed();
    const auto found=std::ranges::find_if(entries,[&](const auto &s){return s.key()==service.key();});
    if (found!=entries.end()) {populate(found-entries.begin()); status->setText(tr("That service is already listed.")); return;}
    entries.append(std::move(service)); populate(entries.size()-1);
}
}

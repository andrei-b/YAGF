#include "ocrservicesdialog.h"
#include <QComboBox>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
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
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <algorithm>

namespace yagf::ui {
using namespace processing;
namespace {
QString modelLabel(const OcrService &s) {
    return s.model.isEmpty() || s.label.contains(s.model,Qt::CaseInsensitive) ? s.label : s.label+" — "+s.model;
}
QString modelChoice(const OcrService &s) {
    const auto location=localOcrProvider(s.provider) ? OcrServicesDialog::tr("this computer") : s.url.authority();
    auto label=modelLabel(s);
    if (!localOcrProvider(s.provider) || !label.contains(location,Qt::CaseInsensitive)) label+=" ("+location+")";
    if (!s.ready && !s.error.isEmpty()) label+=OcrServicesDialog::tr(" — unavailable");
    return label;
}
QString modelDetails(const OcrService &s) {
    const auto state=s.ready ? OcrServicesDialog::tr("Ready") : s.error.isEmpty() ? OcrServicesDialog::tr("Not checked") : OcrServicesDialog::tr("Unavailable: %1").arg(s.error);
    const auto location=localOcrProvider(s.provider) ? OcrServicesDialog::tr("Runs on this computer") : OcrServicesDialog::tr("Remote service: %1").arg(s.url.toString());
    return state+" · "+ocrProviderLabel(s.provider)+'\n'+location;
}
}
OcrServicesDialog::OcrServicesDialog(QVector<OcrService> services,OcrService active,QWidget *parent,OcrService formulas,QString layoutToken)
    :QDialog(parent),entries(std::move(services)),primary(std::move(active)) {
    setObjectName("ocrServicesDialog"); setWindowTitle(tr("Recognition settings")); resize(940,620);
    auto *layout=new QVBoxLayout(this);
    auto *header=new QHBoxLayout;
    auto *intro=new QLabel(tr("Choose a model for text and a separate model for formulas."),this); intro->setWordWrap(true); header->addWidget(intro,1);
    scan=new QPushButton(tr("Refresh models"),this); scan->setObjectName("discoverOcrServices"); header->addWidget(scan);
    stopButton=new QPushButton(tr("Stop"),this); header->addWidget(stopButton); layout->addLayout(header);
    auto *tabs=new QTabWidget(this); tabs->setObjectName("recognitionSettingsTabs"); layout->addWidget(tabs,1);
    auto *recognitionPage=new QWidget(tabs); auto *recognition=new QVBoxLayout(recognitionPage); recognition->setSpacing(18);
    tabs->addTab(recognitionPage,tr("Recognition"));
    auto *textGroup=new QGroupBox(tr("Text recognition"),recognitionPage); auto *textForm=new QFormLayout(textGroup);
    textForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    textEngine=new QComboBox(textGroup); textEngine->setObjectName("textOcrEngine"); textEngine->setMinimumContentsLength(35); textEngine->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    textForm->addRow(tr("Model:"),textEngine);
    textDetails=new QLabel(textGroup); textDetails->setObjectName("textOcrModelDetails"); textDetails->setTextFormat(Qt::PlainText); textDetails->setWordWrap(true); textForm->addRow(textDetails);
    textLanguage=new QLineEdit(textGroup); textLanguage->setObjectName("textOcrLanguage"); textLanguage->setMaxLength(64); textLanguage->setPlaceholderText(tr("Follow the current document language")); textForm->addRow(tr("Language:"),textLanguage);
    auto *textHelp=new QLabel(tr("Used for text blocks and the text in mixed text/formula blocks. Tesseract language examples: rus, eng, rus+eng."),textGroup); textHelp->setWordWrap(true); textForm->addRow(textHelp); recognition->addWidget(textGroup);
    auto *formulaGroup=new QGroupBox(tr("Formula recognition"),recognitionPage); auto *formulaForm=new QFormLayout(formulaGroup);
    formulaForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    formulaEngine=new QComboBox(formulaGroup); formulaEngine->setObjectName("formulaOcrEngine"); formulaEngine->setMinimumContentsLength(35); formulaEngine->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    formulaForm->addRow(tr("Model:"),formulaEngine);
    formulaDetails=new QLabel(formulaGroup); formulaDetails->setObjectName("formulaOcrModelDetails"); formulaDetails->setTextFormat(Qt::PlainText); formulaDetails->setWordWrap(true); formulaForm->addRow(formulaDetails);
    auto *formulaHelp=new QLabel(tr("Used for Formula blocks, equations in Text + formulas blocks, and Math only recognition. Produces LaTeX. Raw OCR output bypasses this model."),formulaGroup); formulaHelp->setWordWrap(true); formulaForm->addRow(formulaHelp); recognition->addWidget(formulaGroup);
    auto *manage=new QPushButton(tr("Manage models and services…"),recognitionPage); manage->setObjectName("manageOcrModels"); recognition->addWidget(manage,0,Qt::AlignLeft); recognition->addStretch();
    auto *layoutPage=new QWidget(tabs); auto *pageLayout=new QVBoxLayout(layoutPage); auto *layoutForm=new QFormLayout;
    layoutForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow); tabs->addTab(layoutPage,tr("Page layout"));
    auto *layoutHelp=new QLabel(tr("Detect text, formulas and pictures before recognition. Select a discovered layout model in Models & services, or enter its service URL below."),layoutPage); layoutHelp->setWordWrap(true); pageLayout->addWidget(layoutHelp);
    auto *servicesPage=new QWidget(tabs); auto *serviceLayout=new QVBoxLayout(servicesPage); tabs->addTab(servicesPage,tr("Models & services"));
    connect(manage,&QPushButton::clicked,this,[tabs,servicesPage]{tabs->setCurrentWidget(servicesPage);});
    auto *serviceHelp=new QLabel(tr("Refresh finds running services on these hosts. Each model appears in the Text and/or Formula selector according to what it recognizes. Optionally select up to three companion text engines for comparison."),servicesPage); serviceHelp->setWordWrap(true); serviceLayout->addWidget(serviceHelp);
    auto *search=new QHBoxLayout;
    hosts=new QLineEdit(QSettings("YAGF","YAGF Revival").value("ocr/discoveryHosts","127.0.0.1").toString(),this); hosts->setObjectName("ocrDiscoveryHosts");
    hosts->setPlaceholderText(tr("127.0.0.1, jetson-host, https://server:8765")); hosts->setToolTip(tr("Host names probe ports 8765–8769, layout on 8771, the gateway on 8772 and PaddleOCR-VL on 8082. A URL or host:port probes that exact service. Existing configured URLs are also checked. Up to 32 endpoints; no document images are sent during discovery."));
    search->addWidget(new QLabel(tr("Hosts / service URLs:"),this)); search->addWidget(hosts,1);
    serviceLayout->addLayout(search);
    table=new QTableWidget(0,7,this); table->setObjectName("ocrServicesTable");
    table->setHorizontalHeaderLabels({tr("Compare"),tr("Name"),tr("Model / engine"),tr("Service URL"),tr("Language"),tr("Status"),tr("Recognizes")});
    table->setSelectionBehavior(QAbstractItemView::SelectRows); table->setSelectionMode(QAbstractItemView::SingleSelection); table->verticalHeader()->hide();
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive); table->horizontalHeader()->setSectionResizeMode(3,QHeaderView::Stretch);
    for (const auto &[column,width]:{std::pair{0,95},{1,195},{2,115},{4,90},{5,145}}) table->setColumnWidth(column,width);
    serviceLayout->addWidget(table,1);
    connect(textEngine,&QComboBox::currentIndexChanged,this,[this](int index) {
        if (filling || index<0) return;
        selectedPrimary=textEngine->itemData(index).toString();
        for (auto &s:entries) if (s.key()==selectedPrimary) s.enabled=false;
        populate(table->currentRow());
    });
    connect(textLanguage,&QLineEdit::textEdited,this,[this](const QString &value) {
        const auto key=textEngine->currentData().toString();
        for (auto &s:entries) if (s.key()==key) {s.language=value.trimmed(); selectedPrimary=key;}
        populate(table->currentRow());
    });
    layoutEndpoint=new QLineEdit(QSettings("YAGF","YAGF Revival").value("layout/url",QSettings().value("layout/url","http://127.0.0.1:8771")).toString(),this);
    layoutEndpoint->setObjectName("pageLayoutServiceUrl"); layoutEndpoint->setPlaceholderText(tr("http://layout-host.example:8771 or a local forward"));
    layoutAuth=std::move(layoutToken);
    connect(layoutEndpoint,&QLineEdit::textChanged,this,[this]{if (!filling) layoutAuth.clear();});
    layoutThreshold=new QDoubleSpinBox(this); layoutThreshold->setObjectName("pageLayoutThreshold"); layoutThreshold->setRange(.1,.95); layoutThreshold->setSingleStep(.05); layoutThreshold->setValue(QSettings("YAGF","YAGF Revival").value("layout/threshold",QSettings().value("layout/threshold",.5)).toDouble());
    layoutForm->addRow(tr("Service URL:"),layoutEndpoint); layoutForm->addRow(tr("Minimum score:"),layoutThreshold);
    automaticLayout=new QCheckBox(tr("Use AI page layout before Tesseract page and batch OCR"),this);
    automaticLayout->setObjectName("automaticPageLayout");automaticLayout->setChecked(QSettings("YAGF","YAGF Revival").value("layout/automatic",false).toBool());
    automaticLayout->setToolTip(tr("Refresh automatic blocks with the selected layout service before OCR. Keep manual blocks. Selected-block recognition uses the selected blocks directly."));
    layoutForm->addRow(automaticLayout); pageLayout->addLayout(layoutForm); pageLayout->addStretch();
    selectedFormula=formulas.key();
    if (formulas.provider!="rapid-latex-ocr" && std::ranges::none_of(entries,[&](const auto &s){return s.key()==formulas.key();})) entries.append(formulas);
    connect(formulaEngine,&QComboBox::currentIndexChanged,this,[this](int index) {if (!filling && index>=0) {selectedFormula=formulaEngine->itemData(index).toString(); refresh();}});
    auto *tools=new QHBoxLayout; add=new QPushButton(tr("Add service…"),this); remove=new QPushButton(tr("Remove"),this); usePrimary=new QPushButton(tr("Use selected for text"),this); usePrimary->setObjectName("usePrimaryOcrService");
    useFormula=new QPushButton(tr("Use for formulas"),this); useFormula->setObjectName("useFormulaOcrService");
    connect(useFormula,&QPushButton::clicked,this,[this] {const int row=table->currentRow(); if (row>=0 && entries[row].supportsFormulas()) {selectedFormula=entries[row].key(); populate(row);}});
    useLayout=new QPushButton(tr("Use selected for layout"),this); useLayout->setObjectName("useLayoutService");
    connect(useLayout,&QPushButton::clicked,this,[this] {const int row=table->currentRow();if (row>=0 && entries[row].supportsPageLayout()) {layoutEndpoint->setText(entries[row].url.toString());layoutAuth=entries[row].token;}});
    tools->addWidget(add); tools->addWidget(remove); tools->addStretch(); tools->addWidget(useLayout); tools->addWidget(useFormula); tools->addWidget(usePrimary); serviceLayout->addLayout(tools);
    auto *auth=new QHBoxLayout; auth->addWidget(new QLabel(tr("Selected service token:"),this)); token=new QLineEdit(this); token->setObjectName("ocrServiceToken"); token->setEchoMode(QLineEdit::Password); token->setPlaceholderText(tr("Optional bearer token; kept only in memory")); auth->addWidget(token,1); serviceLayout->addLayout(auth);
    summary=new QLabel(this); summary->setTextFormat(Qt::PlainText); summary->setWordWrap(true); layout->addWidget(summary);
    status=new QLabel(this); status->setObjectName("ocrDiscoveryStatus"); status->setTextFormat(Qt::PlainText); status->setWordWrap(true); layout->addWidget(status);
    auto *buttons=new QDialogButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Cancel,this); saveButton=buttons->button(QDialogButtonBox::Save); layout->addWidget(buttons);
    if (std::ranges::none_of(entries,[](const auto &s){return s.provider=="tesseract";})) {
        OcrService local; local.provider="tesseract"; local.label=ocrProviderLabel(local.provider); local.ready=!QStandardPaths::findExecutable("tesseract").isEmpty(); entries.prepend(local);
    }
    if (ocrProviders().contains(primary.provider) && std::ranges::none_of(entries,[&](const auto &s){return s.key()==primary.key();})) entries.append(primary);
    for (auto &s:entries) if (s.key()==primary.key()) {s.language=primary.language;s.token=primary.token;}
    connect(table,&QTableWidget::cellChanged,this,[this](int row,int column) {
        if (filling || row<0 || row>=entries.size()) return;
        auto &s=entries[row]; const auto previous=s.key();
        if (column==0) s.enabled=table->item(row,0)->checkState()==Qt::Checked;
        else if (column==1) s.label=table->item(row,1)->text().trimmed();
        else if (column==3) {s.url=QUrl(table->item(row,3)->text().trimmed()); s.token.clear(); s.ready=false;}
        else if (column==4) s.language=table->item(row,4)->text().trimmed();
        if (selectedPrimary==previous || (selectedPrimary.isEmpty() && primary.key()==previous)) selectedPrimary=s.key();
        if (selectedFormula==previous) selectedFormula=s.key();
        const auto textChoice=textEngine->findData(previous);
        if (textChoice>=0) {textEngine->setItemData(textChoice,s.key());textEngine->setItemText(textChoice,modelChoice(s));}
        const auto chosen=formulaEngine->findData(previous);
        if (chosen>=0) {formulaEngine->setItemData(chosen,s.key()); formulaEngine->setItemText(chosen,modelChoice(s));}
        refresh();
    });
    connect(table,&QTableWidget::currentCellChanged,this,[this] {const int row=table->currentRow(); const QSignalBlocker block(token); token->setText(row>=0 && row<entries.size() ? entries[row].token : QString()); refresh();});
    connect(token,&QLineEdit::textEdited,this,[this](const QString &value) {const int row=table->currentRow(); if (row>=0) {entries[row].token=value;if (entries[row].supportsPageLayout() && entries[row].url.toString()==pageLayoutUrl()) layoutAuth=value;}});
    connect(scan,&QPushButton::clicked,this,&OcrServicesDialog::discover); connect(stopButton,&QPushButton::clicked,this,[this]{stop.request_stop();});
    connect(usePrimary,&QPushButton::clicked,this,&OcrServicesDialog::choosePrimary); connect(add,&QPushButton::clicked,this,&OcrServicesDialog::addService);
    connect(remove,&QPushButton::clicked,this,[this] {const int row=table->currentRow(); if (row<0 || localOcrProvider(entries[row].provider)) return; if (entries[row].key()==selectedPrimary || (selectedPrimary.isEmpty() && entries[row].key()==primary.key())) selectedPrimary="local-tesseract"; if (entries[row].key()==selectedFormula) selectedFormula=localFormulaService().key(); entries.removeAt(row); populate();});
    connect(buttons,&QDialogButtonBox::accepted,this,&OcrServicesDialog::save); connect(buttons,&QDialogButtonBox::rejected,this,&OcrServicesDialog::reject);
    connect(&watcher,&QFutureWatcherBase::finished,this,[this] {
        busy=false; auto result=watcher.result();
        if (!result) status->setText(result.error().cancelled ? tr("Discovery stopped.") : result.error().message);
        else {
            for (auto &s:entries) if (!localOcrProvider(s.provider)) {s.ready=false; s.error=tr("No response in this discovery");}
            for (auto discovered:result->services) {
                auto found=std::ranges::find_if(entries,[&](const auto &s){return s.key()==discovered.key();});
                if (found==entries.end()) {if (entries.size()<64) entries.append(std::move(discovered));}
                else {found->ready=discovered.ready; found->error=discovered.error; if (found->language.isEmpty()) found->language=discovered.language; found->tasks=discovered.tasks; found->model=discovered.model;}
            }
            const int ready=std::ranges::count_if(result->services,[](const auto &s){return s.ready;});
            status->setText(tr("%1 remote recognition/layout services ready. Unavailable endpoints can be checked again after starting their service or SSH tunnel.").arg(ready));
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
        auto *check=new QTableWidgetItem; check->setFlags(Qt::ItemIsEnabled|Qt::ItemIsSelectable|Qt::ItemIsUserCheckable); check->setCheckState(s.enabled ? Qt::Checked : Qt::Unchecked); if (!s.supportsText()) check->setFlags(Qt::ItemIsEnabled|Qt::ItemIsSelectable); table->setItem(row,0,check);
        const QStringList values{s.label,s.model.isEmpty() ? ocrProviderLabel(s.provider) : s.model,s.url.toString(),s.language,s.ready ? tr("Ready") : s.error.isEmpty() ? tr("Not checked") : s.error,
            s.supportsPageLayout() ? tr("Page layout") : s.supportsFormulas() ? (s.supportsText() ? tr("Text, formulas") : tr("Formulas")) : tr("Text")};
        for (int column=1;column<7;++column) {
            auto *item=new QTableWidgetItem(values[column-1]); item->setToolTip(values[column-1]);
            if (column==2 || column==5 || column==6 || (column==3 && s.provider=="tesseract")) item->setFlags(item->flags()&~Qt::ItemIsEditable);
            if (column==4) item->setToolTip(tr("An empty local Tesseract language follows the primary language. Remote services normally report their fixed language or auto."));
            table->setItem(row,column,item);
        }
        table->setRowHeight(row,42);
    }
    textEngine->clear();
    for (const auto &s:entries) if (s.supportsText()) {textEngine->addItem(modelChoice(s),s.key()); textEngine->setItemData(textEngine->count()-1,modelDetails(s),Qt::ToolTipRole);}
    textEngine->setCurrentIndex(textEngine->findData(selectedPrimary.isEmpty() ? primary.key() : selectedPrimary));
    formulaEngine->clear();
    const auto local=localFormulaService(); formulaEngine->addItem(modelChoice(local),local.key()); formulaEngine->setItemData(0,modelDetails(local),Qt::ToolTipRole);
    for (const auto &s:entries) if (s.supportsFormulas() && s.provider!="rapid-latex-ocr") {formulaEngine->addItem(modelChoice(s),s.key()); formulaEngine->setItemData(formulaEngine->count()-1,modelDetails(s),Qt::ToolTipRole);}
    formulaEngine->setCurrentIndex(std::max(0,formulaEngine->findData(selectedFormula)));
    selectedFormula=formulaEngine->currentData().toString();
    table->setCurrentCell(entries.isEmpty() ? -1 : std::clamp(selected,0,int(entries.size())-1),1); filling=false; refresh();
}
void OcrServicesDialog::refresh() {
    const int row=table->currentRow(); const bool selected=row>=0 && row<entries.size();
    const auto active=primaryService(); const auto key=active ? active->key() : primary.key();
    const int enabled=std::ranges::count_if(entries,[&](const auto &s){return s.enabled && s.supportsText() && s.key()!=key;});
    const auto text=std::ranges::find_if(entries,[&](const auto &s){return s.key()==key;});
    const auto &textService=text==entries.end() ? primary : *text;
    textDetails->setText(modelDetails(textService)); formulaDetails->setText(modelDetails(formulaService()));
    {const QSignalBlocker block(textLanguage); textLanguage->setText(textService.language);}
    textLanguage->setReadOnly(textService.provider!="tesseract" && textService.provider!="paddleocr");
    textLanguage->setToolTip(textLanguage->isReadOnly() ? tr("Language reported by this model.") : tr("Language used by the selected text model."));
    summary->setText(enabled>3 ? tr("Select at most three companion text engines before saving.") : tr("%1 companion text engines. Changes take effect when you save.").arg(enabled));
    table->setEnabled(!busy); hosts->setEnabled(!busy); scan->setEnabled(!busy); stopButton->setVisible(busy); add->setEnabled(!busy && entries.size()<64);
    remove->setEnabled(!busy && selected && !localOcrProvider(entries[row].provider)); usePrimary->setEnabled(!busy && selected && entries[row].supportsText()); useFormula->setEnabled(!busy && selected && entries[row].supportsFormulas()); formulaEngine->setEnabled(!busy); textEngine->setEnabled(!busy); textLanguage->setEnabled(!busy); layoutEndpoint->setEnabled(!busy); layoutThreshold->setEnabled(!busy); automaticLayout->setEnabled(!busy); useLayout->setEnabled(!busy && selected && entries[row].supportsPageLayout()); token->setEnabled(!busy && selected && !localOcrProvider(entries[row].provider)); saveButton->setEnabled(!busy && enabled<=3);
}
void OcrServicesDialog::discover() {
    if (busy) return; busy=true; stop=std::stop_source{}; const auto cancellation=stop.get_token();
    auto targets=ocrDiscoveryTargets(hosts->text(),entries);
    const auto layoutUrl=QUrl(layoutEndpoint->text().trimmed());
    if ((QSettings("YAGF","YAGF Revival").contains("layout/url") || QSettings().contains("layout/url") || layoutEndpoint->isModified()) && targets.size()<32 && validOcrServiceUrl(layoutUrl) && !targets.contains(layoutUrl)) targets.append(layoutUrl);
    const auto known=entries;
    status->setText(tr("Checking OCR service health…")); refresh();
    watcher.setFuture(QtConcurrent::run([targets,known,cancellation]{return discoverOcrServices(targets,known,cancellation);}));
}
void OcrServicesDialog::choosePrimary() {
    const int row=table->currentRow(); if (row<0 || !entries[row].supportsText()) return; selectedPrimary=entries[row].key(); entries[row].enabled=false; populate(row);
}
std::optional<OcrService> OcrServicesDialog::primaryService() const {
    if (selectedPrimary.isEmpty()) return {};
    const auto found=std::ranges::find_if(entries,[&](const auto &s){return s.key()==selectedPrimary;});
    return found==entries.end() ? std::optional<OcrService>{} : *found;
}
OcrService OcrServicesDialog::formulaService() const {
    const auto found=std::ranges::find_if(entries,[&](const auto &s){return s.key()==selectedFormula && s.supportsFormulas();});
    return found==entries.end() ? localFormulaService() : *found;
}
bool OcrServicesDialog::automaticPageLayout() const {return automaticLayout->isChecked();}
QString OcrServicesDialog::pageLayoutUrl() const {return layoutEndpoint->text().trimmed();}
double OcrServicesDialog::pageLayoutThreshold() const {return layoutThreshold->value();}
void OcrServicesDialog::save() {
    if ((automaticPageLayout() || !pageLayoutUrl().isEmpty()) && !validOcrServiceUrl(QUrl(pageLayoutUrl()))) {status->setText(tr("Enter a valid HTTP(S) page layout service URL.")); return;}
    for (const auto &s:entries) if (s.label.isEmpty() || s.label.size()>128 || s.language.size()>64 || (!localOcrProvider(s.provider) && !validOcrServiceUrl(s.url))) {
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
    auto *provider=new QComboBox(&dialog); for (const auto &p:ocrProviders()) if (!localOcrProvider(p)) provider->addItem(ocrProviderLabel(p),p);
    auto *name=new QLineEdit(&dialog),*url=new QLineEdit("http://127.0.0.1:8765",&dialog),*language=new QLineEdit("auto",&dialog); name->setMaxLength(128); language->setMaxLength(64);
    auto *model=new QLineEdit(&dialog); model->setMaxLength(256); model->setPlaceholderText(tr("Model name for a direct PaddleOCR-VL vision server"));
    form->addRow(tr("Model:"),model);
    form->addRow(tr("Engine:"),provider); form->addRow(tr("Name:"),name); form->addRow(tr("Service URL:"),url); form->addRow(tr("Language:"),language);
    auto *error=new QLabel(&dialog); error->setWordWrap(true); form->addRow(error);
    auto *buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&dialog); form->addRow(buttons);
    connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    connect(buttons,&QDialogButtonBox::accepted,&dialog,[&] {if (!validOcrServiceUrl(QUrl(url->text().trimmed()))) error->setText(tr("Enter an HTTP(S) URL without credentials, query or fragment.")); else if (provider->currentData()=="paddleocr-vl" && model->text().trimmed().isEmpty()) error->setText(tr("Enter the vision server's model name, or use discovery.")); else dialog.accept();});
    if (dialog.exec()!=QDialog::Accepted) return;
    OcrService service; service.provider=provider->currentData().toString(); service.label=name->text().trimmed(); if (service.label.isEmpty()) service.label=ocrProviderLabel(service.provider); service.url=QUrl(url->text().trimmed()); service.language=language->text().trimmed(); service.model=model->text().trimmed();
    const auto found=std::ranges::find_if(entries,[&](const auto &s){return s.key()==service.key();});
    if (found!=entries.end()) {populate(found-entries.begin()); status->setText(tr("That service is already listed.")); return;}
    entries.append(std::move(service)); populate(entries.size()-1);
}
}

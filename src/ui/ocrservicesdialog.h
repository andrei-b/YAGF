#pragma once
#include "ocrservices.h"
#include "ocrlanguagedialog.h"
#include <QDialog>
#include <QFutureWatcher>
#include <optional>
class QTableWidget;
class QBoxLayout;
class QLineEdit;
class QLabel;
class QPushButton;
class QComboBox;
class QDoubleSpinBox;
class QCheckBox;
namespace yagf::ui {
class RecognitionModelPanel;
class OcrServicesDialog final:public QDialog {
    Q_OBJECT
public:
    OcrServicesDialog(QVector<processing::OcrService> services,processing::OcrService primary,QWidget *parent=nullptr,
                      processing::OcrService formulas=processing::localFormulaService(),QString layoutToken={});
    ~OcrServicesDialog() override;
    QVector<processing::OcrService> services() const {return entries;}
    std::optional<processing::OcrService> primaryService() const;
    processing::OcrService formulaService() const;
    QString pageLayoutUrl() const;
    QString pageLayoutToken() const {return layoutAuth;}
    double pageLayoutThreshold() const;
    bool automaticPageLayout() const;
protected:
    void resizeEvent(QResizeEvent *) override;
    void showEvent(QShowEvent *) override;
    void reject() override;
private:
    void discover();
    void refreshLocalLanguages();
    void selectTextModel(const QString &key, std::optional<QString> language = {});
    void populateTextModels();
    void populate(int selected=0);
    void refresh();
    void choosePrimary();
    void addService();
    void save();
    QVector<processing::OcrService> entries;
    processing::OcrService primary;
    QString selectedPrimary;
    QString selectedFormula;
    QComboBox *formulaEngine,*textEngine;
    QLineEdit *layoutEndpoint,*textLanguage;
    QDoubleSpinBox *layoutThreshold;
    QCheckBox *automaticLayout;
    QString layoutAuth;
    QTableWidget *table;
    QLineEdit *hosts,*token;
    QLabel *status,*summary,*comparisonWarning;
    QBoxLayout *modelPanels = nullptr;
    RecognitionModelPanel *textPanel,*formulaPanel;
    QPushButton *scan,*stopButton,*usePrimary,*useFormula,*useLayout,*remove,*saveButton,*add;
    QFutureWatcher<std::expected<processing::OcrDiscovery,processing::OcrError>> watcher;
    QFutureWatcher<QList<OcrLanguageChoice>> languageWatcher;
    QList<OcrLanguageChoice> localLanguages;
    QStringList hiddenLanguages;
    std::stop_source stop;
    bool opened=false,busy=false,filling=false;
};
}

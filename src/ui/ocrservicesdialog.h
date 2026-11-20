#pragma once
#include "ocrservices.h"
#include <QDialog>
#include <QFutureWatcher>
#include <optional>
class QTableWidget;
class QLineEdit;
class QLabel;
class QPushButton;
class QComboBox;
class QDoubleSpinBox;
class QCheckBox;
namespace yagf::ui {
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
    void showEvent(QShowEvent *) override;
    void reject() override;
private:
    void discover();
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
    QLineEdit *layoutEndpoint;
    QDoubleSpinBox *layoutThreshold;
    QCheckBox *automaticLayout;
    QString layoutAuth;
    QTableWidget *table;
    QLineEdit *hosts,*token;
    QLabel *status,*summary;
    QPushButton *scan,*stopButton,*usePrimary,*useLayout,*remove,*saveButton,*add;
    QFutureWatcher<std::expected<processing::OcrDiscovery,processing::OcrError>> watcher;
    std::stop_source stop;
    bool opened=false,busy=false,filling=false;
};
}

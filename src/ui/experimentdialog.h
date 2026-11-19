#pragma once
#include "experimentrunner.h"
#include <QDialog>
#include <QFutureWatcher>
#include <stop_token>
class QTableWidget;
class QSpinBox;
class QLabel;
class QPushButton;
class QProgressBar;
namespace yagf::ui {
class ExperimentDialog final : public QDialog {
    Q_OBJECT
public:
    explicit ExperimentDialog(core::OcrDocument document,QWidget *parent=nullptr);
    ~ExperimentDialog() override;
    void reject() override;
signals:
    void progressChanged(int complete,int total,const QString &message);
private:
    void addPipeline(const QString &label,const QString &provider,int dpi,bool enhance,bool checked);
    void proofreadReference();
    void refreshReferences();
    void start();
    void showResults();
    void saveReport();
    void openReport();
    void inspectReading();
    core::OcrDocument document;
    QTableWidget *references,*pipelines,*results;
    QWidget *setup;
    QSpinBox *repetitions,*warmups;
    QLabel *status;
    QProgressBar *progress;
    QPushButton *run,*cancel,*save,*inspect;
    QFutureWatcher<std::expected<QJsonObject,QString>> watcher;
    std::stop_source stop;
    QJsonObject report;
};
}

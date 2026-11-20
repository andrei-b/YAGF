#pragma once
#include "ocrtrainingdataset.h"
#include <QDialog>
#include <QFutureWatcher>
#include <atomic>
#include <memory>
class QComboBox;
class QCheckBox;
class QLineEdit;
class QSpinBox;
class QLabel;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QProgressBar;
class QTimer;
namespace yagf::ui {
struct NativeTrainingWork {
    QString error;
    processing::NativeTrainingDataset dataset;
    QJsonObject response;
};
class NativeTrainingDialog final : public QDialog {
    Q_OBJECT
public:
    NativeTrainingDialog(core::OcrDocument document,int currentPage,QWidget *parent=nullptr);
    ~NativeTrainingDialog() override;
    QUrl selectedModelUrl() const {return selectedUrl;}
    QString sessionToken() const;
protected:
    void reject() override;
    void showEvent(QShowEvent *event) override;
private:
    void collect();
    void connectHost();
    void loadModels();
    void train();
    void stopWork();
    void poll();
    void refreshSamples(int selected=-1);
    void showSample(int index);
    void saveText();
    void decision(const QString &decision);
    void refreshButtons();
    void showModel(const QJsonObject &model);
    void useModel();
    void exportBundle();
    void importDataset();
    void saveCache();
    void run(QString kind,std::function<NativeTrainingWork()> task);
    void request(QString kind,const QByteArray &method,const QString &path,const QJsonObject &body={});
    void finished();
    core::OcrDocument book;
    processing::NativeTrainingDataset dataset;
    QLineEdit *endpoint,*token,*transcription;
    QComboBox *profiles,*scope,*partition,*models;
    QCheckBox *continueFromModel;
    QSpinBox *first,*last,*iterations;
    QLabel *preview,*message,*hostStatus,*counts;
    QListWidget *samples;
    QPlainTextEdit *comparison;
    QPushButton *trainButton,*useButton,*collectButton,*connectButton,*stopButton,*approveButton,*excludeButton,*approveReviewed;
    QProgressBar *progress;
    QTimer *pollTimer;
    QFutureWatcher<NativeTrainingWork> watcher;
    std::stop_source stop;
    std::shared_ptr<std::atomic_bool> cancelled;
    QString kind,jobId,connectedEndpoint,connectedToken;
    QJsonObject candidate;
    QUrl selectedUrl;
    int editingSample=-1;
    bool cancelAfterSubmit=false;
    bool busy=false,activeJob=false,opened=false,closing=false,initialCollect=true,showing=false;
};
}

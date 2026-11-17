#pragma once

#include "ocrdocument.h"
#include "documentprocessor.h"
#include "pdfexporter.h"

#include <QFutureWatcher>
#include <QGraphicsView>
#include <QMainWindow>
#include <QImage>
#include <QPoint>
#include <QRectF>
#include <QTemporaryDir>

#include <atomic>
#include <memory>

class QGraphicsRectItem;
class QGraphicsScene;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QCloseEvent;
class QSpinBox;
class QTabWidget;
class QTextEdit;
class QRubberBand;
class QAction;

namespace yagf::ui {

class PageView : public QGraphicsView {
    Q_OBJECT
public:
    explicit PageView(QWidget *parent = nullptr);
    void setRegionMode(bool enabled);
    void fitPage();
    void zoomBy(qreal factor);
signals:
    void wordClicked(int index);
    void regionSelected(const QRectF &bounds);
protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
private:
    QRubberBand *rubberBand;
    QPoint regionOrigin;
    bool regionMode = false;
    bool fitMode = true;
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow();
    ~MainWindow() override;
    void loadSource(const QString &path);
protected:
    void closeEvent(QCloseEvent *event) override;
private:
    void openFile();
    void openProject();
    bool saveProject();
    bool saveProjectAs();
    bool maybeSave();
    void markDirty();
    void updateTitle();
    void loadImage(const QString &path);
    void showPage(int number);
    void updatePageControls();
    void runOcr();
    void showDocument();
    void selectWord(int index);
    void selectRegion(const QRectF &bounds);
    void updateSelection(const QVector<int> &indices);
    bool applyCorrection();
    bool maybeApplyEditor();
    void runRegionOcr();
    void runBatchOcr();
    void editLayout();
    void createRegionFromSelection();
    void improveCurrentPage();
    void exportText();
    void exportHtml();
    void exportJson();
    void exportPdf(bool archive);
    void setBusy(bool busy);
    void showError(const QString &message);

    PageView *view;
    QGraphicsScene *scene;
    QListWidget *wordList;
    QListWidget *pageList = nullptr;
    QTextEdit *selectionEditor;
    QLabel *selectionLabel;
    QPushButton *applyButton;
    QPushButton *regionOcrButton;
    QLineEdit *languageEdit;
    QLabel *status;
    QPushButton *ocrButton;
    QPushButton *cancelButton;
    QPushButton *previousButton;
    QPushButton *nextButton;
    QAction *improveAction;
    QAction *batchAction = nullptr;
    QAction *layoutAction = nullptr;
    QAction *createRegionAction = nullptr;
    QAction *pdfAction = nullptr;
    QAction *pdfaAction = nullptr;
    QSpinBox *pageSpin;
    processing::DocumentProcessor processor;
    QVector<QGraphicsRectItem *> wordBoxes;
    QVector<core::OcrWord *> displayedWords;
    QVector<int> selectedWords;
    QRect selectedRegion;
    QGraphicsRectItem *regionOutline = nullptr;
    core::OcrDocument document;
    QString imagePath;
    QString rawImagePath;
    QImage rawImage;
    QTemporaryDir preparedDir;
    QString originalPath;
    QString projectPath;
    bool dirty = false;
    bool editorPending = false;
    bool exportBusy = false;
    QString pdfExportPath;
    std::shared_ptr<std::atomic_bool> pdfCancel;
    QFutureWatcher<processing::PdfExportResult> pdfWatcher;
    int currentPage = 1;
    int pageCount = 1;
};

} // namespace yagf::ui

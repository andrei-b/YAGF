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
#include <QUndoStack>

#include <atomic>
#include <memory>

class QGraphicsRectItem;
class QGraphicsPathItem;
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
class QMimeData;
class QDragEnterEvent;
class QDropEvent;

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
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
signals:
    void inputDropped(const QMimeData *mime);
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
    void loadProject(const QString &path);
protected:
    void closeEvent(QCloseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
private:
    void openFile();
    void scanPage();
    void pasteImage();
    void importMimeData(const QMimeData *mime);
    QString saveImportedImage(const QImage &image);
    void openProject();
    void openPortableProject();
    bool savePortableProject();
    bool saveProject();
    bool saveProjectAs();
    bool maybeSave();
    void markDirty();
    void updateTitle();
    void refreshDirty();
    void loadImage(const QString &path);
    void showPage(int number);
    void updatePageControls();
    void runOcr();
    void configureOcrEngine();
    void showDocument();
    void selectWord(int index);
    void selectRegion(const QRectF &bounds);
    void updateSelection(const QVector<int> &indices);
    QVector<int> indicesForIds(const QStringList &ids) const;
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
    QAction *undoAction = nullptr;
    QAction *redoAction = nullptr;
    QSpinBox *pageSpin;
    processing::DocumentProcessor processor;
    QVector<QGraphicsPathItem *> wordBoxes;
    QVector<core::OcrWord *> displayedWords;
    QVector<int> selectedWords;
    QRect selectedRegion;
    QGraphicsRectItem *regionOutline = nullptr;
    core::OcrDocument document;
    QString imagePath;
    QString rawImagePath;
    QImage rawImage;
    core::PreprocessSettings displayedPreprocessing;
    QTemporaryDir preparedDir;
    QString originalPath;
    QString projectPath;
    bool dirty = false;
    bool externalDirty = false;
    QUndoStack undoStack;
    bool editorPending = false;
    bool exportBusy = false;
    QString pdfExportPath;
    std::shared_ptr<std::atomic_bool> pdfCancel;
    QFutureWatcher<processing::PdfExportResult> pdfWatcher;
    int currentPage = 1;
    int pageCount = 1;
};

} // namespace yagf::ui

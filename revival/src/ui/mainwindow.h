#pragma once

#include "ocrdocument.h"
#include "documentprocessor.h"

#include <QGraphicsView>
#include <QMainWindow>
#include <QPoint>
#include <QRectF>

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
    void exportText();
    void exportHtml();
    void exportJson();
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
    QSpinBox *pageSpin;
    DocumentProcessor processor;
    QVector<QGraphicsRectItem *> wordBoxes;
    QVector<OcrWord *> displayedWords;
    QVector<int> selectedWords;
    QRect selectedRegion;
    QGraphicsRectItem *regionOutline = nullptr;
    OcrDocument document;
    QString imagePath;
    QString originalPath;
    QString projectPath;
    bool dirty = false;
    bool editorPending = false;
    int currentPage = 1;
    int pageCount = 1;
};

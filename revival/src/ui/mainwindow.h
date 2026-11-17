#pragma once

#include "ocrdocument.h"
#include "documentprocessor.h"

#include <QGraphicsView>
#include <QMainWindow>

class QGraphicsRectItem;
class QGraphicsScene;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QCloseEvent;
class QSpinBox;
class QTabWidget;

class PageView : public QGraphicsView {
    Q_OBJECT
public:
    explicit PageView(QWidget *parent = nullptr);
signals:
    void wordClicked(int index);
protected:
    void mousePressEvent(QMouseEvent *event) override;
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
    void exportText();
    void exportJson();
    void setBusy(bool busy);
    void showError(const QString &message);

    PageView *view;
    QGraphicsScene *scene;
    QListWidget *wordList;
    QListWidget *pageList = nullptr;
    QTabWidget *sidebarTabs;
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
    OcrDocument document;
    QString imagePath;
    QString originalPath;
    QString projectPath;
    bool dirty = false;
    int currentPage = 1;
    int pageCount = 1;
};

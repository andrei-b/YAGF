#pragma once

#include "ocrdocument.h"
#include "correctionmemory.h"
#include "verificationqueue.h"

#include <optional>
#include "documentprocessor.h"
#include "exporttypes.h"

#include <QFutureWatcher>
#include <QGraphicsView>
#include <QMainWindow>
#include <QImage>
#include <QPoint>
#include <QPointF>
#include <QRectF>
#include <QSet>
#include <QTemporaryDir>
#include <QTextCursor>
#include <QUndoStack>

#include <atomic>
#include <memory>

class QGraphicsRectItem;
class QGraphicsPathItem;
class QGraphicsScene;
class QLabel;
class QLineEdit;
class QToolButton;
class QListWidget;
class QPushButton;
class QCheckBox;
class QCloseEvent;
class QSpinBox;
class QTabWidget;
class QTextEdit;
class QRubberBand;
class QAction;
class QMenu;
class QMimeData;
class QDragEnterEvent;
class QDropEvent;
class QKeyEvent;
class QUrl;
class QContextMenuEvent;
class QComboBox;
class QProgressDialog;
class QToolBar;
class QTimer;

namespace yagf::ui {

class SpellHighlighter;
class VerificationController;

class PageView : public QGraphicsView {
    Q_OBJECT
public:
    explicit PageView(QWidget *parent = nullptr);
    void setActiveRegionIndex(int index);
    void fitPage();
    void zoomBy(qreal factor);
    QToolBar *imageTools() const { return imageToolbar; }
signals:
    void wordClicked(int index);
    void regionSelected(const QRectF &bounds);
    void regionActivated(int index, bool toggle = false);
    void regionChanged(int index, const QRectF &bounds);
    void regionDeleteRequested(int index);
    void regionContextMenuRequested(int index, const QPoint &globalPosition);
    void pageContextMenuRequested(const QPoint &globalPosition);
protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void scrollContentsBy(int dx, int dy) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    bool viewportEvent(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
signals:
    void inputDropped(const QMimeData *mime);
private:
    void updateRegionDrag(const QPoint &position);
    void positionImageToolbar();
    void updateImageToolbar(const QPoint &position);
    QVector<QRectF> regionObstacles() const;
    QRubberBand *rubberBand;
    QPointF regionAnchor;
    QRect drawingRegion;
    std::optional<QPoint> panPosition;
    bool fitMode = true;
    int activeRegionIndex = -1;
    int editingRegionIndex = -1;
    int resizeEdges = 0;
    QRectF editStartRect;
    QPointF editStartScene;
    QGraphicsRectItem *editingRegionItem = nullptr;
    QToolBar *imageToolbar = nullptr;
    QAction *pinImageToolbar = nullptr;
    QTimer *hideImageToolbar = nullptr;
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow();
    ~MainWindow() override;
    void loadSource(const QString &path);
    void loadProject(const QString &path);
    bool usePaddleVlService(const QUrl &url);
signals:
    void exportProgress(qsizetype completed, qsizetype total);
protected:
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
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
    void rememberProject();
    void refreshRecentProjects();
    QMenu *recentProjectsMenu;
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
    void classifyPageAreas();
    void splitSelectedBlockWithAi();
    QImage displayedPageImage() const;
    void trainNativeOcr();
    void syncNativeOcrModel();
    QString appliedNativeOcrUrl;
    QHash<QString,QString> nativeOcrTokens;
    QAction *nativeTrainingAction = nullptr;
    QAction *aiPageAreasAction = nullptr;
    QAction *aiSplitBlockAction = nullptr;
    void configureOcrEngine();
    void configureOcrServices();
    void updateComparisonEngines();
    void updatePageLayoutService();
    void reviewOcrReadings();
    QVector<processing::OcrService> ocrServices;
    QString layoutSessionToken;
    QAction *ocrServicesAction=nullptr,*ocrReadingsAction=nullptr;
    void configureRendering();
    void openExperiments();
    void activateVerification(core::VerificationItem item, int action = 0);
    void finishVerification();
    void refreshRenderedPageIfNeeded();
    void configureSpecialists();
    void configureDocumentContext();
    void reviewNextSpecialist();
    void chooseOcrLanguages();
    void refreshOcrLanguageList();
    void finishProjectLoading();
    void refreshImageToolbar();
    void editSymbolRules(bool fromSelection);
    bool canReviewSelection() const;
    void reviewSelectedWord();
    void showDocument();
    void selectWord(int index);
    void selectRegion(const QRectF &bounds);
    void activateImageRegion(int index, bool toggle = false);
    void moveImageRegion(int index, const QRectF &bounds);
    void deleteImageRegion(int index);
    void clearPageBlocks(int pageNumber);
    void showPageContextMenu(const QPoint &position);
    void showPageActions(int pageNumber, const QPoint &globalPosition);
    void showBlockContextMenu(int index, const QPoint &globalPosition);
    void copyBlockImage(int index);
    void setBlockLanguage(int index, const QString &language, bool notText);
    QString blockLanguage(const core::ImageRegion &region) const;
    void refreshImageRegionSelection(bool updateWords);
    void runNextImageRegion();
    void refreshReviewLabel();
    void editSelectedFormula();
    void refreshSpellLanguage();
    void updateSelection(const QVector<int> &indices);
    QVector<int> indicesForIds(const QStringList &ids) const;
    bool applyCorrection();
    bool maybeApplyEditor();
    void runBatchOcr(bool remainingOnly);
    void updateRecognitionProgress(qsizetype completed, qsizetype total);
    void editLayout();
    void createRegionFromSelection();
    void improveCurrentPage();
    void correctCurrentPageSkew();
    bool canUndoSkewCorrection() const;
    void restoreCurrentPage();
    void applyPagePreprocessing(const core::PreprocessSettings &proposed, bool geometryChanged,
                                bool recordSkewCorrection = false);
    void exportText();
    void exportHtml();
    void exportJson();
    void exportLatex();
    enum class ExportFormat : quint8 { Pdf, FormattedPdf, ArchivalPdf, Samples, ScientificBook, LatexDocument, ScanImage, ScanPdf };
    void exportPdf(bool archive, bool formatted = false);
    void convertDjVuToPdf();
    void exportVerifiedSamples();
    void exportScientificBook();
    void chooseExportPath(const QString &title, const QString &filter, const QString &suffix, ExportFormat format);
    void startExport(const QString &path, ExportFormat format, bool typesetFormulas);
    void setBusy(bool busy);
    void showError(const QString &message);

    PageView *view;
    QGraphicsScene *scene;
    QListWidget *wordList;
    QListWidget *pageList = nullptr;
    QListWidget *regionList = nullptr;
    QTextEdit *selectionEditor;
    SpellHighlighter *spellHighlighter = nullptr;
    VerificationController *verification = nullptr;
    QAction *renderingAction = nullptr;
    std::optional<core::VerificationItem> verificationTarget;
    int verificationAction = 0;
    QComboBox *spellLanguage = nullptr;
    QAction *spellCheckAction = nullptr;
    QLabel *selectionLabel;
    QPushButton *applyButton;
    QPushButton *regionOcrButton;
    QPushButton *splitBlocksButton;
    QPushButton *reviewWordButton;
    QPushButton *deleteRegionButton = nullptr;
    QLineEdit *languageEdit;
    QToolButton *languagePickerButton;
    QListWidget *ocrLanguageList = nullptr;
    QLabel *ocrLanguageStatus = nullptr;
    QLabel *status;
    QToolButton *ocrButton;
    QAction *recognizeAction = nullptr;
    QAction *mathMode;
    QAction *unicodeFormulasAction = nullptr;
    QPushButton *cancelButton;
    QAction *cancelRecognitionAction = nullptr;
    QAction *cancelExportAction = nullptr;
    QProgressDialog *recognitionProgress = nullptr;
    QPushButton *previousButton;
    QPushButton *nextButton;
    QAction *scanAction;
    QAction *improveAction;
    QAction *restoreAction;
    QAction *batchAction = nullptr;
    QAction *remainingAction = nullptr;
    QAction *layoutAction = nullptr;
    QAction *editFormulaAction = nullptr;
    QAction *createRegionAction = nullptr;
    QAction *scanExportAction = nullptr;
    QAction *pdfAction = nullptr;
    QAction *formattedPdfAction = nullptr;
    QAction *pdfaAction = nullptr;
    QAction *convertDjVuAction = nullptr;
    QAction *undoAction = nullptr;
    QAction *redoAction = nullptr;
    QAction *languagePickerAction = nullptr;
    QSpinBox *pageSpin;
    core::OcrDocument document;
    processing::DocumentProcessor processor{nullptr, &document};
    QVector<QGraphicsPathItem *> wordBoxes;
    QVector<QGraphicsRectItem *> imageRegionBoxes;
    QVector<core::OcrWord *> displayedWords;
    QVector<int> selectedWords;
    struct EditorSection {
        int firstWord;
        int lastWord;
        QTextCursor separator; // Inside the blank line before this section, tracking edits above it.
    };
    QVector<EditorSection> editorSections;
    QHash<int, QPair<int, int>> editorWordRanges;
    QSet<QString> selectedRegionIds;
    int selectedImageRegion = -1;
    QVector<core::ImageRegion> queuedImageRegions;
    int nextQueuedImageRegion = 0;
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
    QString exportPath;
    QString aiReviewToken;
    struct SpecialistService { bool enabled = false; QString url; QString token; };
    SpecialistService formulaService, proseService;
    QAction *nextSpecialistAction = nullptr;
    QAction *similarSymbolsAction = nullptr;
    QAction *symbolRulesAction = nullptr;
    QAction *teachSymbolAction = nullptr;
    QAction *documentContextAction = nullptr;
    QAction *preserveFormattingAction = nullptr;
    QAction *rawOcrOutputAction = nullptr;
    QAction *rotateLeftAction = nullptr;
    QAction *rotateRightAction = nullptr;
    QAction *correctSkewAction = nullptr;
    QAction *undoSkewAction = nullptr;
    QAction *imageSplitAction = nullptr;
    QAction *imageClearBlocksAction = nullptr;
    QAction *imageRecognizeBlockAction = nullptr;
    QAction *imageBlockOptionsAction = nullptr;
    QAction *imageRemoveBlockAction = nullptr;
    QProgressDialog *projectLoadingProgress = nullptr;
    bool projectLoading = false;
    std::optional<core::CorrectionMemory> correctionMemory;
    std::shared_ptr<std::atomic_bool> exportCancel;
    QFutureWatcher<processing::ExportResult> exportWatcher;
    int currentPage = 1;
    int pageCount = 1;
};

} // namespace yagf::ui

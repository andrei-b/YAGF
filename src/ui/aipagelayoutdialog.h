#pragma once
#include "aipagelayout.h"
#include "regiongeometry.h"
#include <QDialog>
#include <QFutureWatcher>
#include <QHash>
#include <stop_token>
class QLineEdit;
class QSlider;
class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QToolButton;
class QListWidget;
class QListWidgetItem;
class QFrame;
class QGraphicsScene;
class QGraphicsView;
class QGraphicsRectItem;
class QProgressBar;
namespace yagf::ui {
class AiPageLayoutDialog : public QDialog {
    Q_OBJECT
public:
    AiPageLayoutDialog(QImage image, QVector<core::ImageRegion> existing, QWidget *parent=nullptr, int pageNumber=0,
                       QString bearerToken={}, bool blockScope=false, double deskewAdjustment=0,
                       bool hasAppliedLayout=false);
    ~AiPageLayoutDialog() override;
    QVector<core::ImageRegion> regions() const;
    const QVector<core::ImageRegion> &proposedRegions() const { return initialRegions; }
    QString sessionToken() const;
    void enableHistory(processing::OcrOptions context,QString sourcePath,QJsonObject preprocessing);
    core::RecognitionHistory recordedHistory();
protected:
    void reject() override;
    void showEvent(QShowEvent *event) override;
    bool eventFilter(QObject *object, QEvent *event) override;
private:
    struct BlockCard {
        QFrame *frame;
        QCheckBox *use;
        QComboBox *type;
        QLabel *score;
        QToolButton *alternative;
    };
    void analyze();
    void populate();
    void updateSelection();
    void updateCard(int index);
    void focusBlock(QListWidgetItem *item);
    void fitPreview();
    void filterAreas();
    QImage image;
    core::RegionInk ink;
    QVector<QRect> localInlineHints;
    QVector<core::ImageRegion> existing;
    QVector<core::ImageRegion> initialRegions;
    QVector<processing::PageArea> areas;
    QVector<processing::PageArea> allAreas;
    QHash<QString,QString> typeOverrides;
    QHash<QString,bool> selectionOverrides;
    QVector<QGraphicsRectItem *> boxes;
    QVector<QGraphicsRectItem *> preservedBoxes;
    QVector<QListWidgetItem *> areaItems;
    QVector<BlockCard> cards;
    QLineEdit *endpoint, *token;
    QSlider *threshold;
    QLabel *thresholdValue;
    QCheckBox *replaceAutomatic;
    QLabel *message, *blockCount, *emptyHint;
    QPushButton *analyzeButton, *applyButton;
    QToolButton *optionsButton;
    QProgressBar *progress;
    QListWidget *miniatures;
    QGraphicsScene *scene;
    QGraphicsView *preview;
    QRectF previewTarget;
    bool updating=false;
    bool busy=false;
    bool opened=false;
    bool hasAnalysis=false;
    bool blockScope=false;
    bool hasAppliedLayout=false;
    std::stop_source stop;
    processing::OcrOptions historyContext;
    QString historySourcePath;
    QFutureWatcher<std::expected<processing::PageLayout,processing::OcrError>> watcher;
};
}

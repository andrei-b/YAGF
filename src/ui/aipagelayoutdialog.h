#pragma once
#include "aipagelayout.h"
#include <QDialog>
#include <QFutureWatcher>
#include <stop_token>
class QLineEdit;
class QDoubleSpinBox;
class QCheckBox;
class QLabel;
class QPushButton;
class QTableWidget;
class QGraphicsScene;
class QGraphicsView;
class QGraphicsRectItem;
class QProgressBar;
namespace yagf::ui {
class AiPageLayoutDialog : public QDialog {
    Q_OBJECT
public:
    AiPageLayoutDialog(QImage image, QVector<core::ImageRegion> existing, QWidget *parent=nullptr);
    ~AiPageLayoutDialog() override;
    QVector<core::ImageRegion> regions() const;
protected:
    void reject() override;
    void resizeEvent(QResizeEvent *event) override;
private:
    void analyze();
    void populate();
    void updateSelection(int changed=-1);
    QImage image;
    QVector<core::ImageRegion> existing;
    QVector<processing::PageArea> areas;
    QVector<QGraphicsRectItem *> boxes;
    QVector<QGraphicsRectItem *> preservedBoxes;
    QLineEdit *endpoint, *token;
    QDoubleSpinBox *threshold;
    QCheckBox *replaceAutomatic;
    QLabel *message;
    QPushButton *analyzeButton, *applyButton;
    QProgressBar *progress;
    QTableWidget *table;
    QGraphicsScene *scene;
    QGraphicsView *preview;
    bool updating=false;
    bool busy=false;
    std::stop_source stop;
    QFutureWatcher<std::expected<processing::PageLayout,processing::OcrError>> watcher;
};
}

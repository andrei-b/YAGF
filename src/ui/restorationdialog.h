#pragma once

#include "pagerestoration.h"
#include <QDialog>
#include <QPixmap>
#include <functional>

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QTimer;

namespace yagf::ui {
class RestorationCanvas;
class RestorationDialog : public QDialog {
public:
    RestorationDialog(QImage original, QImage base, QWidget *parent);
    const QImage &resultImage() const { return working; }
    bool geometryChanged() const { return straightened; }

private:
    void updateControls();
    void refreshPage();
    void updateDetail();
    void mergeCapture();
    void run(std::function<processing::restoration::Result()> operation, bool changesGeometry);

    QImage original, base, working;
    QPixmap thumbnail, focusThumbnail;
    RestorationCanvas *canvas;
    QComboBox *tool, *profile;
    QCheckBox *focus;
    QWidget *deblurControls;
    QSpinBox *angle, *length, *strength;
    QPushButton *applyRegion, *applyGuides, *clearGuides, *merge, *reset;
    QLabel *before, *after, *status;
    QDialogButtonBox *buttons;
    QTimer *previewTimer;
    bool busy = false, edited = false, straightened = false;
};
}

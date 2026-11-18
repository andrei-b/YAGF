#include "layoutdialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

namespace yagf::ui {

LayoutDialog::LayoutDialog(core::OcrPage sourcePage, const QImage &image, QWidget *parent)
    : QDialog(parent), page(std::move(sourcePage)),
      previewBase(image.scaled(QSize(500, 580), Qt::KeepAspectRatio, Qt::SmoothTransformation))
{
    setWindowTitle(tr("Edit page layout and reading order"));
    resize(1000, 700);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(tr("Select a region, adjust its box and type, or move it in reading order."), this));
    auto *content = new QHBoxLayout();
    previewLabel = new QLabel(this);
    previewLabel->setAlignment(Qt::AlignCenter);
    previewLabel->setMinimumSize(520, 590);
    previewLabel->setStyleSheet(QStringLiteral("background:white;border:1px solid #aaa"));
    content->addWidget(previewLabel, 2);
    auto *right = new QVBoxLayout();
    regionList = new QListWidget(this);
    regionList->setObjectName(QStringLiteral("layoutRegions"));
    right->addWidget(regionList, 2);
    auto *orderButtons = new QHBoxLayout();
    upButton = new QPushButton(tr("Move up"), this);
    downButton = new QPushButton(tr("Move down"), this);
    orderButtons->addWidget(upButton);
    orderButtons->addWidget(downButton);
    right->addLayout(orderButtons);
    auto *form = new QFormLayout();
    typeCombo = new QComboBox(this);
    typeCombo->setEditable(true);
    typeCombo->addItems({QStringLiteral("text"), QStringLiteral("heading"),
                         QStringLiteral("column"), QStringLiteral("table"),
                         QStringLiteral("image"), QStringLiteral("footnote")});
    form->addRow(tr("Type"), typeCombo);
    xSpin = new QSpinBox(this);
    ySpin = new QSpinBox(this);
    widthSpin = new QSpinBox(this);
    heightSpin = new QSpinBox(this);
    xSpin->setRange(0, page.width - 1);
    ySpin->setRange(0, page.height - 1);
    widthSpin->setRange(1, page.width);
    heightSpin->setRange(1, page.height);
    form->addRow(tr("X"), xSpin);
    form->addRow(tr("Y"), ySpin);
    form->addRow(tr("Width"), widthSpin);
    form->addRow(tr("Height"), heightSpin);
    right->addLayout(form);
    right->addStretch();
    content->addLayout(right, 1);
    layout->addLayout(content);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Apply layout"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    connect(regionList, &QListWidget::currentRowChanged, this, [this](int row) { selectRegion(row); });
    connect(upButton, &QPushButton::clicked, this, [this] { moveRegion(-1); });
    connect(downButton, &QPushButton::clicked, this, [this] { moveRegion(1); });
    connect(typeCombo, &QComboBox::currentTextChanged, this, [this] { applyFields(); });
    for (QSpinBox *spin : {xSpin, ySpin, widthSpin, heightSpin})
        connect(spin, &QSpinBox::valueChanged, this, [this] { applyFields(); });
    fillList(0);
}

void LayoutDialog::fillList(int selected)
{
    const QSignalBlocker blocker(regionList);
    regionList->clear();
    for (const core::OcrRegion &region : page.regions) {
        int words = 0;
        for (const auto &paragraph : region.paragraphs)
            for (const auto &line : paragraph.lines) words += line.wordIds.size();
        regionList->addItem(tr("%1 · %2 (%3 words)")
            .arg(region.id.section(QLatin1Char('/'), -1)).arg(region.type).arg(words));
    }
    regionList->setCurrentRow(selected);
    selectRegion(selected);
}

void LayoutDialog::selectRegion(int index)
{
    const bool valid = index >= 0 && index < page.regions.size();
    for (QWidget *widget : QList<QWidget *>{typeCombo, xSpin, ySpin, widthSpin, heightSpin})
        widget->setEnabled(valid);
    upButton->setEnabled(valid && index > 0);
    downButton->setEnabled(valid && index + 1 < page.regions.size());
    if (valid) {
        const auto &region = page.regions.at(index);
        const QSignalBlocker blockType(typeCombo);
        const QSignalBlocker blockX(xSpin), blockY(ySpin), blockW(widthSpin), blockH(heightSpin);
        typeCombo->setCurrentText(region.type);
        xSpin->setValue(region.bounds.x());
        ySpin->setValue(region.bounds.y());
        widthSpin->setMaximum(page.width - region.bounds.x());
        heightSpin->setMaximum(page.height - region.bounds.y());
        widthSpin->setValue(region.bounds.width());
        heightSpin->setValue(region.bounds.height());
    }
    refreshPreview();
}

void LayoutDialog::applyFields()
{
    const int index = regionList->currentRow();
    if (index < 0 || index >= page.regions.size()) return;
    const QSignalBlocker blockW(widthSpin), blockH(heightSpin);
    widthSpin->setMaximum(page.width - xSpin->value());
    heightSpin->setMaximum(page.height - ySpin->value());
    core::OcrRegion &region = page.regions[index];
    const QRect bounds(xSpin->value(), ySpin->value(), widthSpin->value(), heightSpin->value());
    QString type = typeCombo->currentText().trimmed();
    if (type.isEmpty()) type = QStringLiteral("text");
    if (region.bounds == bounds && region.type == type) return;
    region.bounds = bounds;
    region.type = type;
    changed = true;
    regionList->item(index)->setText(tr("%1 · %2")
        .arg(region.id.section(QLatin1Char('/'), -1), region.type));
    refreshPreview();
}

void LayoutDialog::moveRegion(int direction)
{
    const int index = regionList->currentRow();
    if (!page.moveRegion(index, direction)) return;
    changed = true;
    fillList(index + direction);
}

void LayoutDialog::refreshPreview()
{
    if (previewBase.isNull() || page.width <= 0 || page.height <= 0) return;
    QImage overlay = previewBase.convertToFormat(QImage::Format_ARGB32);
    QPainter painter(&overlay);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.scale(static_cast<double>(overlay.width()) / page.width,
                  static_cast<double>(overlay.height()) / page.height);
    for (int i = 0; i < page.regions.size(); ++i) {
        const bool selected = i == regionList->currentRow();
        painter.setPen(QPen(selected ? QColor(220, 60, 20) : QColor(20, 90, 220), selected ? 4 : 2));
        painter.setBrush(selected ? QColor(220, 60, 20, 45) : QColor(20, 90, 220, 25));
        painter.drawRect(page.regions.at(i).bounds);
    }
    previewLabel->setPixmap(QPixmap::fromImage(overlay));
}

} // namespace yagf::ui

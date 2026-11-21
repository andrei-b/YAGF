#include "restorationdialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QtConcurrentRun>
#include <algorithm>
#include <cmath>

namespace yagf::ui {
namespace restore = processing::restoration;

enum Tool { Inspect, Deblur, Trace };

class RestorationCanvas : public QWidget {
public:
    explicit RestorationCanvas(QSize size, QWidget *parent)
        : QWidget(parent), pageSize(size), point(QRect(QPoint(), size).center())
    {
        setMinimumSize(220, 230);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setCursor(Qt::CrossCursor);
    }
    QSize pageSize;
    QPoint point;
    QRect region;
    QVector<QPolygonF> guides;
    QPixmap thumbnail;
    Tool mode = Inspect;
    std::function<void()> changed;

protected:
    QRectF paper() const
    {
        const auto scaled = QSizeF(pageSize).scaled(size(), Qt::KeepAspectRatio);
        return {(width()-scaled.width())/2, (height()-scaled.height())/2, scaled.width(), scaled.height()};
    }
    QPoint pixel(QPointF widgetPoint) const
    {
        const auto r = paper();
        return {std::clamp(int((widgetPoint.x()-r.left()) * pageSize.width()/r.width()), 0, pageSize.width()-1),
                std::clamp(int((widgetPoint.y()-r.top()) * pageSize.height()/r.height()), 0, pageSize.height()-1)};
    }
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), palette().dark());
        const auto r = paper();
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.drawPixmap(r, thumbnail, thumbnail.rect());
        p.translate(r.topLeft());
        p.scale(r.width()/pageSize.width(), r.height()/pageSize.height());
        QPen pen(QColor("#008ccc"), 2);
        pen.setCosmetic(true);
        p.setPen(pen);
        if (!region.isEmpty()) p.drawRect(region);
        pen.setColor(QColor("#00a873"));
        p.setPen(pen);
        for (const auto &line : guides) p.drawPolyline(line);
        p.drawPolyline(stroke);
        pen.setColor(QColor("#d86300"));
        p.setPen(pen);
        const auto radius = 5 * pageSize.width()/r.width();
        p.drawEllipse(QPointF(point), radius, radius);
    }
    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() != Qt::LeftButton || !paper().contains(e->position())) return;
        dragging = true;
        anchor = point = pixel(e->position());
        if (mode == Trace) stroke = QPolygonF{QPointF(anchor)};
        update();
    }
    void moveStroke(QPoint pos)
    {
        if (mode == Trace && !stroke.isEmpty() && pos.x() > stroke.last().x())
            stroke.append(pos);
        if (mode == Deblur && (anchor-pos).manhattanLength() >= 4) {
            region = QRect(anchor,pos).normalized();
            point = region.center();
        }
        update();
    }
    void mouseMoveEvent(QMouseEvent *e) override
    {
        if (dragging) moveStroke(pixel(e->position()));
    }
    void mouseReleaseEvent(QMouseEvent *e) override
    {
        if (!dragging || e->button() != Qt::LeftButton) return;
        moveStroke(pixel(e->position()));
        dragging = false;
        if (mode == Trace && stroke.size() >= 2 && guides.size() < 12) guides.append(stroke);
        stroke.clear();
        update();
        changed();
    }
private:
    bool dragging = false;
    QPoint anchor;
    QPolygonF stroke;
};

RestorationDialog::RestorationDialog(QImage source, QImage initial, QWidget *parent)
    : QDialog(parent), original(std::move(source)), base(std::move(initial)), working(base)
{
    setWindowTitle(tr("Restore damaged page"));
    geometry.sourceSize=base.size();
    resize(920, 740);
    auto *layout = new QVBoxLayout(this);
    auto *row = new QHBoxLayout;
    canvas = new RestorationCanvas(working.size(), this);
    canvas->setObjectName("restorationCanvas");
    row->addWidget(canvas, 1);
    auto *panel = new QVBoxLayout;
    row->addLayout(panel);
    layout->addLayout(row, 1);

    tool = new QComboBox;
    tool->setObjectName("restorationTool");
    tool->addItems({tr("Inspect detail"), tr("Deblur region"), tr("Straighten text lines")});
    panel->addWidget(tool);
    auto *hint = new QLabel;
    hint->setWordWrap(true);
    hint->setMaximumWidth(305);
    panel->addWidget(hint);
    focus = new QCheckBox(tr("Mark soft detail in red"));
    panel->addWidget(focus);
    deblurControls = new QWidget;
    auto *form = new QFormLayout(deblurControls);
    form->setContentsMargins(0, 0, 0, 0);
    auto spin = [&](int min, int max, int value, const QString &unit) {
        auto *s = new QSpinBox;
        s->setRange(min, max);
        s->setValue(value);
        s->setSuffix(unit);
        return s;
    };
    angle = spin(-90, 90, 0, tr("°"));
    length = spin(1, 64, 7, tr(" px"));
    strength = spin(0, 100, 50, tr("%"));
    profile = new QComboBox;
    profile->addItems({tr("Soft focus"), tr("Motion smear")});
    form->addRow(tr("Blur type"), profile);
    form->addRow(tr("Blur direction"), angle);
    form->addRow(tr("Blur length"), length);
    form->addRow(tr("Strength"), strength);
    applyRegion = new QPushButton(tr("Apply deblur to region"));
    applyRegion->setObjectName("applyRegion");
    form->addRow(applyRegion);
    panel->addWidget(deblurControls);
    applyGuides = new QPushButton(tr("Straighten traced lines"));
    clearGuides = new QPushButton(tr("Clear guides"));
    panel->addWidget(applyGuides);
    panel->addWidget(clearGuides);

    auto detail = [&](const QString &caption) {
        panel->addWidget(new QLabel(caption));
        auto *label = new QLabel;
        label->setFixedSize(300, 112);
        label->setAlignment(Qt::AlignCenter);
        panel->addWidget(label);
        return label;
    };
    before = detail(tr("Original scan · 100%"));
    after = detail(tr("Restoration preview · 100%"));
    panel->addStretch();
    merge = new QPushButton(tr("Merge second capture…"));
    reset = new QPushButton(tr("Discard edits from this session"));
    panel->addWidget(merge);
    panel->addWidget(reset);
    status = new QLabel;
    status->setWordWrap(true);
    layout->addWidget(status);
    buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    previewTimer = new QTimer(this);
    previewTimer->setSingleShot(true);
    connect(previewTimer, &QTimer::timeout, this, &RestorationDialog::updateDetail);
    connect(profile, &QComboBox::currentIndexChanged, this, [this]{ previewTimer->start(90); });
    for (auto *s : {angle, length, strength})
        connect(s, &QSpinBox::valueChanged, this, [this]{ previewTimer->start(90); });
    auto selectTool = [this, hint] {
        canvas->mode = static_cast<Tool>(tool->currentIndex());
        const QStringList hints{tr("Click the page to compare the same location at full resolution."),
            tr("Drag across the blurred region. Click a word to preview the current settings. Zero degrees follows a horizontal smear."),
            tr("Drag left to right along the baselines of two to twelve text lines. Follow their curves as far across the page as possible.")};
        hint->setText(hints.at(tool->currentIndex()));
        updateControls();
        updateDetail();
    };
    connect(tool, &QComboBox::currentIndexChanged, this, selectTool);
    connect(focus, &QCheckBox::toggled, this, &RestorationDialog::refreshPage);
    canvas->changed = [this]{ updateControls(); updateDetail(); };
    connect(clearGuides, &QPushButton::clicked, this, [this] {
        canvas->guides.clear(); canvas->update(); updateControls();
    });
    connect(reset, &QPushButton::clicked, this, [this] {
        working = base;
        geometry.steps.clear();
        straightened = edited = false;
        canvas->guides.clear();
        canvas->region = {};
        thumbnail = focusThumbnail = {};
        status->clear(); refreshPage(); updateControls(); updateDetail();
    });
    connect(merge, &QPushButton::clicked, this, &RestorationDialog::mergeCapture);
    connect(applyGuides, &QPushButton::clicked, this, [this] {
        auto mapping=restore::straighteningGeometry(working.size(),canvas->guides);
        if(!mapping){status->setText(mapping.error());return;}
        run([page=working, guides=canvas->guides]{ return restore::straighten(page, guides); }, true,*mapping);
    });
    connect(applyRegion, &QPushButton::clicked, this, [this] {
        run([page=working, region=canvas->region, len=length->value(), dir=angle->value(), s=strength->value(),
             model=profile->currentIndex()==0 ? restore::BlurProfile::SoftFocus : restore::BlurProfile::Motion] {
            return restore::deblur(page, region, len, dir, s/100., {}, model);
        }, false);
    });
    refreshPage();
    selectTool();
}

void RestorationDialog::updateControls()
{
    const bool tracing = canvas->mode == Trace;
    deblurControls->setVisible(canvas->mode == Deblur);
    applyGuides->setVisible(tracing);
    clearGuides->setVisible(tracing);
    applyGuides->setEnabled(!busy && canvas->guides.size() >= 2);
    clearGuides->setEnabled(!busy && !canvas->guides.isEmpty());
    applyRegion->setEnabled(!busy && !canvas->region.isEmpty());
    reset->setEnabled(!busy && edited);
    for (QWidget *w : QList<QWidget *>{canvas, tool, focus, merge, deblurControls}) w->setEnabled(!busy);
    buttons->button(QDialogButtonBox::Ok)->setEnabled(!busy && edited);
}

void RestorationDialog::refreshPage()
{
    auto miniature = [](const QImage &im) {
        return QPixmap::fromImage(im.scaled(im.size().boundedTo({1200,1200}), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    };
    if (thumbnail.isNull()) thumbnail = miniature(working);
    if (focus->isChecked() && focusThumbnail.isNull()) {
        const auto heat = restore::sharpnessMap(working);
        if (heat) focusThumbnail = miniature(*heat);
        else status->setText(heat.error());
    }
    canvas->thumbnail = focus->isChecked() && !focusThumbnail.isNull() ? focusThumbnail : thumbnail;
    canvas->update();
}

void RestorationDialog::updateDetail()
{
    const qreal dpr = devicePixelRatioF();
    QSize cropSize = (before->size()*dpr).boundedTo(working.size());
    QRect crop(QPoint(std::clamp(canvas->point.x()-cropSize.width()/2, 0, working.width()-cropSize.width()),
                      std::clamp(canvas->point.y()-cropSize.height()/2, 0, working.height()-cropSize.height())), cropSize);
    auto show = [dpr](QLabel *label, const QImage &image) {
        auto pix = QPixmap::fromImage(image);
        pix.setDevicePixelRatio(dpr);
        label->setPixmap(pix);
    };
    show(before, original.copy(crop));
    if (canvas->mode == Deblur && canvas->region.intersects(crop)) {
        const auto result = restore::deblur(working, canvas->region, length->value(), angle->value(), strength->value()/100., crop,
            profile->currentIndex()==0 ? restore::BlurProfile::SoftFocus : restore::BlurProfile::Motion);
        if (result) { show(after, *result); return; }
        status->setText(result.error());
    }
    show(after, working.copy(crop));
}

void RestorationDialog::run(std::function<restore::Result()> operation, bool changesGeometry,core::CoordinatePipeline mapping)
{
    busy = true;
    updateControls();
    status->setText(tr("Processing page…"));
    auto *watcher = new QFutureWatcher<restore::Result>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, changesGeometry,mapping] {
        auto result = watcher->result();
        watcher->deleteLater();
        busy = false;
        if (result) {
            if(mapping.isDefined()) {
                auto combined=geometry;combined.steps+=mapping.steps;
                if(auto valid=combined.validate();!valid){status->setText(valid.error());updateControls();return;}
                geometry=std::move(combined);
            }
            working = std::move(*result);
            edited = true;
            straightened |= changesGeometry;
            canvas->region = {};
            if (changesGeometry) canvas->guides.clear();
            thumbnail = focusThumbnail = {};
            refreshPage();
            status->setText(tr("Edit applied. Click OK to use this page in the project."));
        } else status->setText(result.error());
        updateControls(); updateDetail();
    });
    watcher->setFuture(QtConcurrent::run(std::move(operation)));
}

void RestorationDialog::mergeCapture()
{
    const auto path = QFileDialog::getOpenFileName(this, tr("Second capture of this page"), {},
        tr("Images (*.jpg *.jpeg *.png *.tif *.tiff *.bmp *.webp);;All files (*)"));
    if (path.isEmpty()) return;
    run([page=working, path]() -> restore::Result {
        QImageReader reader(path);
        reader.setAutoTransform(true);
        const auto size = reader.size();
        if (size.isEmpty() || qint64(size.width())*size.height() > 80'000'000)
            return std::unexpected(QStringLiteral("Unsupported capture dimensions."));
        const auto second = reader.read();
        if (second.isNull()) return std::unexpected(reader.errorString());
        return restore::mergeCaptures(page, second);
    }, false);
}
}

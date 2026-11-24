#include "mainwindow.h"
#include "pagesource.h"
#include "formulatext.h"

#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QProcess>
#include <QPushButton>
#include <QTemporaryDir>
#include <QVBoxLayout>
#include <algorithm>

namespace yagf::ui {

void MainWindow::reviewProseTypeface()
{
    if (processor.isBusy() || exportBusy || projectLoading || imagePath.isEmpty()) return;
    const auto model = QFileDialog::getOpenFileName(this, tr("Open prose typeface model"), {},
                                                    tr("Typeface models (*.json *.json.gz)"));
    if (model.isEmpty()) return;
    QImage image = displayedPageImage();
    if (processing::isPagedSource(originalPath) && !image.isNull()) {
        const int dotsPerMeter = qRound(processor.renderedDpi(currentPage) / .0254);
        image.setDotsPerMeterX(dotsPerMeter);
        image.setDotsPerMeterY(dotsPerMeter);
    }
    QTemporaryDir work;
    const auto input = work.filePath("page.png");
    const auto output = work.filePath("findings.json");
    QImage searchImage = image;
    if (const auto *page = document.page(currentPage); page && QSize(page->width, page->height) == image.size()) {
        QRegion excluded;
        for (const auto &block : page->imageRegions)
            if (block.content == "math" || block.notText || block.content == "picture" || block.content == "table")
                excluded |= block.shape();
        for (const auto &region : page->regions)
            if (region.type == "formula" || region.type == "equation" || region.type == "table" || region.isFigure())
                excluded |= region.bounds;
        for (const auto &word : page->words) {
            if (word.geometryInferred) continue;
            const auto source = core::formulaSourceText(word).trimmed();
            const auto spans = core::mathTextSpans(source);
            if (spans.size() == 1 && spans.first().length == source.size())
                excluded |= word.bounds;
        }
        if (!excluded.isEmpty()) {
            searchImage = image.convertToFormat(QImage::Format_RGB32);
            QPainter mask(&searchImage);
            mask.setClipRegion(excluded);
            mask.fillRect(searchImage.rect(), Qt::white);
        }
    }
    if (!work.isValid() || image.isNull() || !searchImage.save(input)) {
        showError(tr("Could not prepare the page image for typeface review."));
        return;
    }
    QDialog dialog(this);
    dialog.setObjectName("proseTypefaceReview");
    dialog.setWindowTitle(tr("Math letters in running text"));
    dialog.resize(700, 480);
    QVBoxLayout layout(&dialog);
    QLabel status(tr("Looking for isolated letters in running text…"));
    status.setWordWrap(true);
    status.setTextFormat(Qt::PlainText);
    QCheckBox unknown(tr("Include characters with no prose reference"));
    QListWidget list;
    QLabel preview;
    preview.setAlignment(Qt::AlignCenter);
    QDialogButtonBox buttons(QDialogButtonBox::Close);
    auto *locate = buttons.addButton(tr("Locate on page"), QDialogButtonBox::AcceptRole);
    locate->setEnabled(false);
    for (QWidget *widget : QList<QWidget *>{&status, &unknown, &list, &preview, &buttons})
        layout.addWidget(widget);

    QJsonArray findings;
    QRect selected;
    const auto fill = [&] {
        list.clear();
        for (const auto &value : findings) {
            const auto item = value.toObject();
            const bool unmodeled = item["status"].toString() == "unknown";
            if (unmodeled && !unknown.isChecked()) continue;
            const auto coords = item["box"].toArray();
            const QRect rect(coords.at(0).toInt(), coords.at(1).toInt(),
                             coords.at(2).toInt()-coords.at(0).toInt(), coords.at(3).toInt()-coords.at(1).toInt());
            if (rect.isEmpty() || !image.rect().contains(rect)) continue;
            QString kind = unmodeled ? tr("No reference") : tr("Unusual shape");
            if (item["status"].toString() == "formula_match")
                kind = tr("Math letter %1 · OCR read").arg(item["suggested"].toString());
            const auto context = item["context"].toString(item["word"].toString());
            auto *entry = new QListWidgetItem(tr("%1   %2   ·   %3").arg(kind, item["char"].toString(), context), &list);
            entry->setData(Qt::UserRole, rect);
        }
        if (list.count()) list.setCurrentRow(0);
    };
    connect(&list, &QListWidget::currentItemChanged, &dialog, [&](QListWidgetItem *item) {
        selected = item ? item->data(Qt::UserRole).toRect() : QRect{};
        locate->setEnabled(!selected.isEmpty());
        preview.clear();
        if (selected.isEmpty()) return;
        const int height = std::max(12, selected.height());
        const auto area = selected.adjusted(-10*height, -height, 10*height, height).intersected(image.rect());
        auto crop = image.copy(area).convertToFormat(QImage::Format_RGB32);
        {
            QPainter painter(&crop);
            painter.setPen(QPen(QColor(210, 65, 25), 2));
            painter.drawRect(selected.translated(-area.topLeft()).adjusted(-2, -2, 2, 2));
        }
        preview.setPixmap(QPixmap::fromImage(crop).scaled(620, 125, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    });
    connect(&unknown, &QCheckBox::toggled, &dialog, fill);
    connect(&buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QProcess process;
    connect(&process, &QProcess::errorOccurred, &dialog, [&] { status.setText(process.errorString()); });
    connect(&process, &QProcess::finished, &dialog, [&](int exitCode, QProcess::ExitStatus result) {
        if (exitCode || result != QProcess::NormalExit) {
            const auto detail = QString::fromUtf8(process.readAllStandardError()).trimmed();
            status.setText(tr("Typeface review failed. %1").arg(detail.isEmpty() ? process.errorString() : detail));
            return;
        }
        QFile file(output);
        if (!file.open(QIODevice::ReadOnly)) { status.setText(file.errorString()); return; }
        const auto report = QJsonDocument::fromJson(file.readAll()).object();
        if (!report["findings"].isArray()) { status.setText(tr("The typeface review report is invalid.")); return; }
        findings = report["findings"].toArray();
        const auto counts = report["counts"].toObject();
        status.setText(tr("%1 math letter matches, %2 unusual shapes, %3 ordinary letters, %4 characters without a reference. Skipped %5 letters in words.")
                       .arg(counts["formula_match"].toInt()).arg(counts["unusual"].toInt())
                       .arg(counts["ordinary"].toInt()).arg(counts["unknown"].toInt()).arg(counts["word_letters"].toInt()));
        fill();
    });
    process.start("python3", {QCoreApplication::applicationDirPath() + "/yagf2-prose-typeface", "inspect",
                              model, input, output, "--scope", "prose"});
    const auto result = dialog.exec();
    if (process.state() != QProcess::NotRunning) { process.kill(); process.waitForFinished(); }
    if (result == QDialog::Accepted && !selected.isEmpty()) {
        const qreal current = view->transform().m11();
        const qreal readable = std::min(8., 32. / selected.height());
        if (readable > current && current > 0) view->zoomBy(readable / current);
        view->centerOn(selected.center());
    }
}

} // namespace yagf::ui

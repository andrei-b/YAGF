#pragma once

#include "exporttypes.h"
#include "ocrdocument.h"
#include <QRegion>
#include <QImage>
#include <QSet>
#include <QTemporaryDir>
#include <atomic>
#include <expected>

namespace yagf::processing::detail {

struct PdfFormula {
    QRect bounds;
    QString source;
    bool display;
    bool passage = false;
    int fontSizePt = 0;
};

struct PdfFormulaPage {
    QVector<PdfFormula> formulas;
    QSet<QString> wordIds;
    int dpiX = 150;
    int dpiY = 150;
};

PdfFormulaPage collectPdfFormulas(const core::OcrPage &page, const QRegion &protectedGraphics);
struct FormulaSpan {
    int word;
    qsizetype start;
    qsizetype length;
    bool display;
};
QVector<FormulaSpan> editableFormulaSpans(const core::OcrPage &page, const QVector<int> &selection);
std::expected<QString, QString> safeLatexMath(const QString &source, bool allowUnicode = false);
std::expected<QImage, QString> renderFormulaPreview(const QString &source, bool display,
                                                    const std::atomic_bool &cancelled);
ExportResult typesetPdfFormulas(const QVector<PdfFormulaPage> &pages, const QTemporaryDir &directory,
                               const std::atomic_bool &cancelled);

}

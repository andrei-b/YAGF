#pragma once

#include "exporttypes.h"
#include "ocrdocument.h"
#include <QRegion>
#include <QSet>
#include <QTemporaryDir>
#include <atomic>

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
ExportResult typesetPdfFormulas(const QVector<PdfFormulaPage> &pages, const QTemporaryDir &directory,
                               const std::atomic_bool &cancelled);

}

#pragma once

#include "preprocesssettings.h"
#include "documentcontext.h"
#include "rendersettings.h"
#include "symbolrules.h"
#include "recognitionresult.h"
#include "recognitionhistory.h"
#include "layouttraining.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QRect>
#include <QRegion>
#include <QImage>
#include <QString>
#include <QStringList>
#include <QVector>

namespace yagf::core {

struct SpecialistReading {
    QString engine;
    QString text;
    QStringList sourceTokens;
    bool formula = false;
    QStringList historyIds;
};

struct OcrWord {
    int page = 1;
    int block = 0;
    int paragraph = 0;
    int line = 0;
    QRect bounds;
    QString originalText;
    QString text;
    double confidence = -1;
    bool reviewed = false;
    QString id;
    QStringList sourceWordIds;
    bool bold = false;
    bool italic = false;
    int fontSizePt = 0;
    int sourceFontSizePt = 0;
    QVector<QRect> sourceBoxes; // Exact image fragments; empty for inserted text.
    bool layoutBlock = false; // A single OCR item can contain paragraphs, LaTeX or a table.
    QByteArray symbolShape; // 16 by 16 normalized ink mask for isolated OCR symbols.
    SpecialistReading alternative;
    QString readingEngine; // Specialist credited for a verbatim, accepted reading.
    QString fontFamily; // Closest installed face; empty keeps the editor/export default.
    int sourceFontSizePx = 0;
    QString latexText; // Canonical formula-bearing text retained for Unicode output.
    bool geometryInferred = false; // Editor input extent, not model-localized geometry.
    bool atOcrCropEdge = false; // Localized ink touched the recognition crop boundary.
    [[nodiscard]] QString fontFamilyCss() const;
};

struct OcrLine {
    QString id;
    QRect bounds;
    QStringList wordIds;
};

struct OcrParagraph {
    QString id;
    QRect bounds;
    QVector<OcrLine> lines;
};

struct OcrRegion {
    QString id;
    QRect bounds;
    QString type = QStringLiteral("text");
    QVector<OcrParagraph> paragraphs;
    // Optional header and span metadata, aligned with the tab-separated block text.
    QJsonArray tableLayout;
    [[nodiscard]] bool isFigure() const;
    static bool validTableLayout(const QJsonValue &value);
};

// User-drawn OCR areas exist before recognition and survive OCR page rebuilds.
struct ImageRegion {
    QString id;
    QRect bounds;
    QString language; // Empty inherits the page recognition language.
    bool notText = false;
    QString content; // Empty inherits the page engine. Otherwise text, math, mixed or picture.
    bool autoDetected = false;
    QString detectedLabel; // Original layout-model category, retained for inspection.
    double detectionScore = -1;
    QVector<QRect> parts; // Relative to bounds.topLeft(). Empty denotes the whole rectangle.
    QRegion shape() const;
    void setShape(const QRegion &pixels);
    void resizeBounds(QRect target);
    QImage crop(const QImage &page) const;
};

struct OcrReadingItem {
    QRect bounds;
    QString text;
    int block = 0, paragraph = 0, line = 0;
    bool layoutBlock = false;
    bool geometryInferred = false;
    QString historyId; // The exact crop/run that produced this item.
};

// Independent engine output. It never replaces the primary text automatically.
struct OcrReading {
    QString source, engine, label, language, error;
    QRect area;
    QVector<OcrReadingItem> items;
    QVector<RecognitionResult> recognitionEvidence;
    QStringList historyIds; // Stable references into the page's immutable run history.
};

struct OcrPage {
    QString id;
    int number = 1;
    int width = 0;
    int height = 0;
    int sourceWidth = 0;
    int sourceHeight = 0;
    PreprocessSettings preprocessing;
    QString engineId;
    QString language;
    QVector<OcrWord> words;
    QVector<OcrRegion> regions;
    QVector<ImageRegion> imageRegions;
    int renderDpi = 150;
    QVector<OcrWord> deletedWords; // Source-linked deletions survive a recognition rerun.
    QVector<OcrWord> pendingCorrections; // Ambiguous retention is reviewed explicitly.
    QVector<OcrReading> ocrReadings;
    QVector<RecognitionResult> recognitionEvidence; // Immutable model output, separate from edits.
    RecognitionHistory recognitionHistory;
    LayoutTraining layoutTraining;
    CoordinatePipeline coordinates;
    QHash<QString, GeometryAnchor> sourceGeometry;
    [[nodiscard]] QVector<int> specialistSpan(int start) const;
    void rebuildHierarchy();
    bool moveRegion(int index, int direction);
    bool extractRegion(const QVector<int> &wordIndices);
};

struct DocumentPageSource {
    QString path;
    int page = 1;
};

class OcrDocument {
public:
    QString sourcePath;
    // Empty for an unmodified single source; otherwise one entry per logical page.
    QVector<DocumentPageSource> pageSources;
    DocumentPageSource sourceForPage(int number) const;
    void materializePageSources();
    QString engine;
    QString language;
    int totalPages = 1;
    DocumentContext context;
    bool preserveFormatting = false;
    bool useRawOcrOutput = false;
    bool mathOcr = false;
    bool unicodeFormulas = false;
    RenderSettings rendering;
    QVector<OcrPage> pages;
    QVector<SymbolRule> symbolRules;
    QString nativeOcrUrl; // Validated model endpoint; authentication remains session-only.

    static bool fromJson(const QJsonObject &json, OcrDocument *result, QString *error);
    OcrPage *page(int number);
    [[nodiscard]] const OcrPage *page(int number) const;
    void replacePage(OcrPage replacement);
    void removePage(int number);
    [[nodiscard]] qsizetype wordCount() const;
    [[nodiscard]] QString plainText() const;
    [[nodiscard]] QString layoutHtml() const;
    [[nodiscard]] QJsonObject toJson(bool includeRestoredPixels = true) const;
};

} // namespace yagf::core

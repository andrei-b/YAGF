#pragma once
#include "ocrdocument.h"
#include <optional>

namespace yagf::intelligence {

enum class Kind { Page, Area, Region, Paragraph, Line, Word };
enum class Relation { Contains, Precedes, Overlaps, CaptionOf, Continues, RefersTo };

struct Node {
    Kind kind;
    QString id, label;
    int page;
    QRect bounds;
    int word = -1;
    QString anchor;
};
struct Edge {
    int from, to;
    Relation relation;
    bool confirmed = false;
};
struct PageGraph {
    QVector<Node> nodes;
    QVector<Edge> edges;
    QVector<int> words; // Current page word index to graph node.
};
struct Hypothesis {
    QString text;
    QStringList evidence, runIds;
    std::optional<double> inkDistance;
    bool rejected = false;
    double score = 0; // Relative ranking only.
};
struct Assessment {
    QVector<Hypothesis> choices;
    QString suggested, task;
    QRect requestCrop;
};

// A review uses one immutable project snapshot. Reopening after edits or Undo rebuilds all evidence.
class DocumentIntelligence {
public:
    DocumentIntelligence(const core::OcrDocument& document, int pageNumber);
    PageGraph graph;
    [[nodiscard]] std::expected<core::OcrPage, QString> setConfirmed(int edge, bool value) const;
    [[nodiscard]] std::expected<core::OcrPage, QString> reject(int word, const Hypothesis& choice) const;
    [[nodiscard]] Assessment assess(int wordIndex, const QImage& scan) const;
    // The caller supplies compatible, available model keys in preference order.
    [[nodiscard]] QString preferredModel(const QStringList& keys, const QString& task) const;
    [[nodiscard]] std::expected<core::OcrPage, QString> accept(int wordIndex, const Hypothesis& choice) const;

private:
    struct Example {
        int page;
        QString id, text, previous, next, language;
    };
    core::OcrDocument snapshot;
    int number;
    QMultiHash<QString, Example> corrections;
    QSet<int> references;
    QVector<Example> coordinateExamples;
    struct PageStyle {
        QString family;
        bool soften = false;
        int samples = 0;
    };
    [[nodiscard]] PageStyle styleFor(const core::OcrPage& page, const QImage& scan, int target) const;
    mutable QVector<int> styleWords;
    mutable qint64 styleImageKey = 0;
    mutable PageStyle cachedStyle;
};
}

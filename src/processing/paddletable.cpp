#include "paddletable.h"
#include <QJsonArray>
#include <QRegularExpression>

namespace yagf::processing {
std::expected<core::TableRegion, QString> parsePaddleTable(const QString &source)
{
    const auto invalid = std::unexpected(QStringLiteral("The OCR service returned an incomplete table grid."));
    static const QRegularExpression marker("<(fcel|ecel|ched|rhed|srow|lcel|ucel|xcel)>");
    struct Cell { QRect grid; QString text; bool header; int occupiedSlots = 1; };
    QVector<Cell> cells;
    QVector<int> previous;
    int width = -1;
    auto rows = source.trimmed().split("<nl>");
    if (!rows.isEmpty() && rows.last().trimmed().isEmpty()) rows.removeLast();
    if (rows.isEmpty() || rows.size() > 4096) return invalid;
    for (int r = 0; r < rows.size(); ++r) {
        const QString row = rows[r].trimmed();
        auto tokens = marker.globalMatch(row);
        if (!tokens.hasNext() || tokens.peekNext().capturedStart() != 0) return invalid;
        QVector<int> current;
        while (tokens.hasNext()) {
            auto match = tokens.next();
            int column = current.size();
            if (column >= 64 || cells.size() >= 100000) return invalid;
            auto end = tokens.hasNext() ? tokens.peekNext().capturedStart() : row.size();
            const auto content = row.mid(match.capturedEnd(), end-match.capturedEnd()).simplified();
            auto kind = match.captured(1);
            if (kind == "ucel" || kind == "lcel" || kind == "xcel") {
                if (!content.isEmpty()) return invalid;
                const int above = column < previous.size() ? previous[column] : -1;
                const int left = current.isEmpty() ? -1 : current.last();
                const int owner = kind == "lcel" ? left : above;
                if (owner < 0 || (kind == "xcel" && left != above)) return invalid;
                cells[owner].grid |= QRect(column, r, 1, 1);
                ++cells[owner].occupiedSlots;
                current.append(owner);
            } else {
                current.append(cells.size());
                cells.append({{column, r, 1, 1}, content, kind == "ched" || kind == "rhed" || kind == "srow"});
            }
        }
        if (width < 0) width = current.size();
        if (current.size() != width) return invalid;
        previous = std::move(current);
    }
    QJsonArray layout;
    QStringList text;
    int i = 0;
    for (int r = 0; r < rows.size(); ++r) {
        QStringList fields;
        QJsonArray metadata;
        while (i < cells.size() && cells[i].grid.top() == r) {
            const auto &cell = cells[i++];
            if (cell.occupiedSlots != cell.grid.width() * cell.grid.height() || cell.grid.height() > 999) return invalid;
            fields.append(cell.text);
            metadata.append(QJsonObject{{"header", cell.header}, {"rowspan", cell.grid.height()}, {"colspan", cell.grid.width()}});
        }
        if (fields.isEmpty()) return invalid;
        text.append(fields.join('\t'));
        layout.append(metadata);
    }
    core::TableRegion table;
    table.text = text.join('\n');
    table.structure["table_layout"] = layout;
    return table;
}
}

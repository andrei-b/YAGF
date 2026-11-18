#pragma once

#include <QByteArray>
#include <QMultiMap>
#include <QVector>
#include <array>

namespace yagf::core {

// Index the existing 16 × 16 project masks by ink area to narrow each search.
class SymbolMatcher {
public:
    void insert(const QByteArray &mask);
    [[nodiscard]] QVector<QByteArray> similar(const QByteArray &mask) const;

private:
    using Rows = std::array<quint16, 16>;
    struct Template {
        QByteArray key;
        Rows rows;
        Rows neighborhood;
    };
    QMultiMap<int, Template> byInkArea;
};

} // namespace yagf::core

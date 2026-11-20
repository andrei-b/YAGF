#include "symbolrules.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <algorithm>
#include <bit>
#include <cmath>

namespace yagf::core {
namespace {
QJsonArray box(const QRect &r) { return {r.x(), r.y(), r.width(), r.height()}; }
QRect rectangle(const QJsonValue &v)
{
    const auto a = v.toArray();
    if (a.size() != 4) return {};
    for (auto n : a) if (!n.isDouble() || n.toDouble() != n.toInt(-1)) return {};
    const QRect r(a[0].toInt(), a[1].toInt(), a[2].toInt(), a[3].toInt());
    return r.x() >= 0 && r.y() >= 0 && r.width() > 0 && r.height() > 0 &&
        qint64(r.x())+r.width() <= 20000 && qint64(r.y())+r.height() <= 20000 ? r : QRect{};
}
QByteArray decoded(const QJsonValue &v) { return QByteArray::fromBase64(v.toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors); }
bool validMask(const QByteArray &m)
{
    if (m.size() != 32) return false;
    int ink = 0;
    for (unsigned char b : m) ink += std::popcount(b);
    return ink >= 4 && ink < 250;
}
bool validModel(const QJsonObject &m)
{
    if (m.isEmpty()) return true;
    if (m["algorithm"] != "symbol-prototypes/1" || m["training_hash"].toString().size() != 64 ||
        !m["radius"].isDouble() || m["radius"].toDouble() < .02 || m["radius"].toDouble() > .3) return false;
    for (const auto key : {"positive", "negative"}) {
        const auto a = m[key].toArray();
        if (a.isEmpty() || a.size() > maximumSymbolExamples) return false;
        for (auto v : a) if (!v.isString() || !validMask(decoded(v))) return false;
    }
    return true;
}
}

QJsonArray symbolRulesJson(const QVector<SymbolRule> &rules)
{
    QJsonArray result;
    for (const auto &r : rules) {
        QJsonArray examples;
        for (const auto &e : r.examples) examples.append(QJsonObject{{"id", e.id}, {"page", e.page},
            {"box", box(e.bounds)}, {"png", QString::fromLatin1(e.png.toBase64())},
            {"mask", QString::fromLatin1(e.mask.toBase64())}, {"reading", e.reading}, {"source", e.source}, {"decision", e.decision}});
        result.append(QJsonObject{{"id", r.id}, {"label", r.label}, {"language", r.language},
            {"enabled", r.enabled}, {"seed_png", QString::fromLatin1(r.seedPng.toBase64())},
            {"seed_box", box(r.seedBounds)}, {"seed_page", r.seedPage}, {"seed_dpi", r.seedDpi},
            {"examples", examples}, {"model", r.model}});
    }
    return result;
}

std::expected<QVector<SymbolRule>, QString> readSymbolRules(const QJsonValue &value)
{
    if (!value.isArray() || value.toArray().size() > maximumSymbolRules)
        return std::unexpected(QStringLiteral("Invalid symbol recognition rule list."));
    QVector<SymbolRule> rules;
    QSet<QString> ids;
    qint64 pixels = 0;
    for (const auto v : value.toArray()) {
        if (!v.isObject()) return std::unexpected(QStringLiteral("Invalid symbol rule."));
        const auto o = v.toObject();
        SymbolRule r;
        r.id = o["id"].toString(); r.label = o["label"].toString(); r.language = o["language"].toString();
        r.enabled = o["enabled"].toBool(); r.seedPng = decoded(o["seed_png"]);
        r.seedBounds = rectangle(o["seed_box"]); r.seedPage = o["seed_page"].toInt(); r.seedDpi = o["seed_dpi"].toInt();
        r.model = o["model"].toObject();
        if (r.id.isEmpty() || r.id.size() > 100 || ids.contains(r.id) || r.label.trimmed().isEmpty() ||
            r.label.size() > 32 || r.language.size() > 64 || !o["enabled"].isBool() ||
            r.seedPng.isEmpty() || r.seedPng.size() > 262144 || r.seedBounds.isEmpty() ||
            r.seedPage < 1 || r.seedPage > 100000 || r.seedDpi < 72 || r.seedDpi > 600 ||
            !o["examples"].isArray() || o["examples"].toArray().size() > maximumSymbolExamples ||
            !o["model"].isObject() || !validModel(r.model))
            return std::unexpected(QStringLiteral("Invalid symbol rule fields."));
        ids.insert(r.id);
        QSet<QString> exampleIds;
        pixels += r.seedPng.size();
        for (const auto ev : o["examples"].toArray()) {
            const auto e = ev.toObject();
            SymbolExample s;
            s.id = e["id"].toString(); s.page = e["page"].toInt(); s.bounds = rectangle(e["box"]);
            s.png = decoded(e["png"]); s.mask = decoded(e["mask"]); s.reading = e["reading"].toString();
            s.decision = e["decision"].toString();
            s.source = e["source"].toString();
            if (s.id.isEmpty() || s.id.size() > 100 || exampleIds.contains(s.id) || s.page < 1 || s.page > 100000 ||
                s.bounds.isEmpty() || s.png.isEmpty() || s.png.size() > 65536 || !validMask(s.mask) ||
                s.reading.size() > 256 || s.source.size() > 4096 || (s.decision != "pending" && s.decision != "positive" && s.decision != "negative"))
                return std::unexpected(QStringLiteral("Invalid symbol training example."));
            exampleIds.insert(s.id); pixels += s.png.size(); r.examples.append(std::move(s));
        }
        if (pixels > 32*1024*1024) return std::unexpected(QStringLiteral("Symbol training images exceed 32 MiB."));
        rules.append(std::move(r));
    }
    return rules;
}

QByteArray symbolTrainingHash(const SymbolRule &rule)
{
    QJsonArray approved;
    for (const auto &e : rule.examples) if (e.decision != "pending")
        approved.append(QJsonArray{QString::fromLatin1(e.mask.toBase64()), e.decision});
    return QCryptographicHash::hash(QJsonDocument(QJsonObject{{"label", rule.label},
        {"language", rule.language}, {"examples", approved}}).toJson(QJsonDocument::Compact),
        QCryptographicHash::Sha256).toHex();
}

bool symbolRuleTrained(const SymbolRule &r)
{
    return !r.model.isEmpty() && validModel(r.model) &&
        r.model["training_hash"].toString().toLatin1() == symbolTrainingHash(r);
}

double symbolDistance(const QByteArray &a, const QByteArray &b)
{
    if (!validMask(a) || !validMask(b)) return 1;
    int different = 0, ink = 0;
    for (int i = 0; i < 32; ++i) {
        const auto x = static_cast<unsigned char>(a[i]), y = static_cast<unsigned char>(b[i]);
        different += std::popcount(static_cast<unsigned char>(x^y));
        ink += std::popcount(static_cast<unsigned char>(x|y));
    }
    return ink ? double(different)/ink : 1;
}

std::expected<QJsonObject, QString> trainSymbolRule(const SymbolRule &r)
{
    QJsonArray positive, negative;
    QVector<QByteArray> masks;
    for (const auto &e : r.examples) {
        if (e.decision == "pending") continue;
        if (!validMask(e.mask)) return std::unexpected(QStringLiteral("A training crop has no usable symbol."));
        if (e.decision == "positive") { positive.append(QString::fromLatin1(e.mask.toBase64())); masks.append(e.mask); }
        else if (e.decision == "negative") negative.append(QString::fromLatin1(e.mask.toBase64()));
    }
    if (positive.size() < 2 || negative.isEmpty())
        return std::unexpected(QStringLiteral("Approve at least two matching examples and one non-matching example before training."));
    double spread = .04;
    for (int i = 0; i < masks.size(); ++i) {
        double nearest = 1;
        for (int j = 0; j < masks.size(); ++j) if (i != j) nearest = std::min(nearest, symbolDistance(masks[i], masks[j]));
        spread = std::max(spread, nearest*1.15);
        for (auto n : negative) if (symbolDistance(masks[i], decoded(n)) < .04)
            return std::unexpected(QStringLiteral("Matching and non-matching examples conflict. Review the labels or choose a more precise crop."));
    }
    return QJsonObject{{"algorithm", "symbol-prototypes/1"}, {"training_hash", QString::fromLatin1(symbolTrainingHash(r))},
        {"positive", positive}, {"negative", negative}, {"radius", std::clamp(spread, .04, .3)}};
}

double symbolRuleScore(const SymbolRule &r, const QByteArray &mask)
{
    if (!r.enabled || !symbolRuleTrained(r)) return 0;
    double positive = 1, negative = 1;
    for (auto v : r.model["positive"].toArray()) positive = std::min(positive, symbolDistance(mask, decoded(v)));
    for (auto v : r.model["negative"].toArray()) negative = std::min(negative, symbolDistance(mask, decoded(v)));
    if (positive > r.model["radius"].toDouble() || negative < positive+.04) return 0;
    return 1-positive;
}

QString symbolLibraryPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)+QStringLiteral("/yagf/symbol-rules.json");
}
std::expected<QVector<SymbolRule>, QString> loadSymbolLibrary(const QString &path)
{
    QFile file(path.isEmpty() ? symbolLibraryPath() : path);
    if (!file.exists()) return QVector<SymbolRule>{};
    if (file.size() > 48*1024*1024 || !file.open(QIODevice::ReadOnly)) return std::unexpected(QStringLiteral("Could not read symbol rule library."));
    QJsonParseError error;
    const auto json = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || json.object()["protocol"] != "yagf-symbol-rules/1")
        return std::unexpected(QStringLiteral("Invalid symbol rule library."));
    return readSymbolRules(json.object()["rules"]);
}
std::expected<void, QString> saveSymbolLibrary(const QVector<SymbolRule> &rules, const QString &path)
{
    const QString target = path.isEmpty() ? symbolLibraryPath() : path;
    const auto json = symbolRulesJson(rules);
    if (auto valid = readSymbolRules(json); !valid) return std::unexpected(valid.error());
    if (!QDir().mkpath(QFileInfo(target).absolutePath())) return std::unexpected(QStringLiteral("Could not create symbol library directory."));
    QSaveFile file(target);
    const auto data = QJsonDocument(QJsonObject{{"protocol", "yagf-symbol-rules/1"}, {"rules", json}}).toJson();
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) return std::unexpected(file.errorString());
    return {};
}
} // namespace yagf::core

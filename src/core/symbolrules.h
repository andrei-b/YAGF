#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QRect>
#include <QString>
#include <QVector>
#include <expected>

namespace yagf::core {

struct SymbolExample {
    QString id;
    int page = 1;
    QRect bounds;
    QByteArray png, mask;
    QString reading;
    QString source;
    QString decision = QStringLiteral("pending"); // positive, negative, or pending
};

struct SymbolRule {
    QString id, label, language;
    bool enabled = true;
    QByteArray seedPng;
    QRect seedBounds;
    int seedPage = 1, seedDpi = 150;
    QVector<SymbolExample> examples;
    QJsonObject model;
};

constexpr int maximumSymbolRules = 128;
constexpr int maximumSymbolExamples = 256;
QJsonArray symbolRulesJson(const QVector<SymbolRule> &rules);
std::expected<QVector<SymbolRule>, QString> readSymbolRules(const QJsonValue &value);
QByteArray symbolTrainingHash(const SymbolRule &rule);
bool symbolRuleTrained(const SymbolRule &rule);
double symbolDistance(const QByteArray &left, const QByteArray &right);
std::expected<QJsonObject, QString> trainSymbolRule(const SymbolRule &rule);
double symbolRuleScore(const SymbolRule &rule, const QByteArray &mask);
QString symbolLibraryPath();
std::expected<QVector<SymbolRule>, QString> loadSymbolLibrary(const QString &path = {});
std::expected<void, QString> saveSymbolLibrary(const QVector<SymbolRule> &rules, const QString &path = {});

} // namespace yagf::core

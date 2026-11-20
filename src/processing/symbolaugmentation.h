#pragma once
#include "ocrdocument.h"
#include <QImage>
#include <stop_token>

namespace yagf::processing {
QByteArray symbolCropPng(const QImage &image, int maximum = 128);
QByteArray symbolFeature(const QImage &image);
struct SymbolCandidate { QRect bounds; QByteArray mask; double score = 0; };
QVector<SymbolCandidate> findSymbolCandidates(const QImage &image, const core::SymbolRule &rule,
                                             int dpi, std::stop_token stop = {}, int maximum = 48);
struct SymbolCut { QString text; QRect bounds; int start = 0; int length = 0; };
QVector<SymbolCut> symbolCuts(const QImage &image, const core::OcrWord &word,
                            const QString &language, std::stop_token stop = {});
int applySymbolRules(core::OcrPage &page, const QImage &image, const QVector<core::SymbolRule> &rules,
                     std::stop_token stop = {});
bool symbolLanguageMatches(const QString &rule, const QString &language);
} // namespace yagf::processing

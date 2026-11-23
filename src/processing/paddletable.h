#pragma once
#include "recognitionresult.h"

namespace yagf::processing {
std::expected<core::TableRegion, QString> parsePaddleTable(const QString &source);
}

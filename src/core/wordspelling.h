#pragma once

#include "ocrdocument.h"
#include <QSet>

namespace yagf::core {
struct WordSpelling {
    QVector<QStringList> tokens;
    QSet<int> continuations;
};

// Dictionary tokens in reading order. Line-end hyphen fragments share their
// joined spelling without changing the document's text or source geometry.
WordSpelling wordSpelling(const OcrPage &page);
}

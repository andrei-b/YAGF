#pragma once

#include "ocrdocument.h"

namespace yagf::core {

class ProjectFile {
public:
    static bool save(const QString &path, const OcrDocument &document, QString *error);
    static bool load(const QString &path, OcrDocument *document, QString *error);
};

} // namespace yagf::core

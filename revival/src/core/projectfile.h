#pragma once

#include "ocrdocument.h"

namespace yagf::core {

class ProjectFile {
public:
    static bool save(const QString &path, const OcrDocument &document, QString *error);
    static bool load(const QString &path, OcrDocument *document, QString *error);
    static bool savePortable(const QString &directory, const OcrDocument &document, QString *error);
    static bool loadPortable(const QString &directory, OcrDocument *document, QString *error);
};

} // namespace yagf::core

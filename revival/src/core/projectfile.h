#pragma once

#include "ocrdocument.h"

class ProjectFile {
public:
    static bool save(const QString &path, const OcrDocument &document, QString *error);
    static bool load(const QString &path, OcrDocument *document, QString *error);
};

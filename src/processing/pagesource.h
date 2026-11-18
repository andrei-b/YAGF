#pragma once

#include <QString>
#include <QStringList>

namespace yagf::processing {

enum class PageSourceKind { Image, Pdf, DjVu };
PageSourceKind pageSourceKind(const QString &path);
bool isPagedSource(const QString &path);

struct SourceCommand {
    QString program;
    QStringList arguments;
    QString outputPath;
};

SourceCommand pageCountCommand(const QString &path);
SourceCommand pageRenderCommand(const QString &path, int page, const QString &outputPrefix,
                               bool thumbnail = false);

} // namespace yagf::processing

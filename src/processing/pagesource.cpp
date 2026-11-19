#include "pagesource.h"
#include "rendersettings.h"

#include <QFileInfo>

namespace yagf::processing {

PageSourceKind pageSourceKind(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == "pdf") return PageSourceKind::Pdf;
    if (suffix == "djvu" || suffix == "djv") return PageSourceKind::DjVu;
    return PageSourceKind::Image;
}

bool isPagedSource(const QString &path) { return pageSourceKind(path) != PageSourceKind::Image; }

SourceCommand pageCountCommand(const QString &path)
{
    if (pageSourceKind(path) == PageSourceKind::DjVu) return {"djvused", {path, "-e", "n"}, {}};
    return {"pdfinfo", {path}, {}};
}

SourceCommand pageRenderCommand(const QString &path, int page, const QString &prefix, bool thumbnail, int dpi)
{
    if (!core::validRenderDpi(dpi)) dpi = 150;
    if (pageSourceKind(path) == PageSourceKind::DjVu) {
        const QString output = prefix + ".ppm";
        return {"ddjvu", {"-format=ppm", "-page=" + QString::number(page),
                          thumbnail ? QStringLiteral("-size=160x160") : QStringLiteral("-scale=%1").arg(dpi),
                          path, output}, output};
    }
    QStringList arguments{"-f", QString::number(page), "-l", QString::number(page), "-singlefile"};
    arguments += thumbnail ? QStringList{"-scale-to", "160"} : QStringList{"-r", QString::number(dpi)};
    arguments += QStringList{"-png", path, prefix};
    return {"pdftoppm", arguments, prefix + ".png"};
}

} // namespace yagf::processing

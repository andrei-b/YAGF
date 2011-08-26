#include "ghostscr.h"
#include <QStringList>
#include <QString>

GhostScr::GhostScr() : PDFExtractor()
{
}

void GhostScr::exec()
{
    QString command = "gs";
    QStringList args;
    args << "-SDEVICE=jpeg" << "-r1200x1200" << "-sPAPERSIZE=letter" << "-dNOPAUSE" << "-dBATCH";
    if ((!getStopPage().isEmpty()) || (getStopPage().toInt() > 0)) {
        if (getStartPage().toInt() == 0)
              this->setStartPage("1");
        args << QString("-dFirstPage=").append(getStartPage()) << QString("-dLastPage=").append(getStopPage());
    }
    if (!getOutputDir().endsWith("/"))
        setOutputDir(getOutputDir().append('/'));
    setOutputPrefix(getOutputDir().append("page"));
    args << QString("-sOutputFile=").append(getOutputPrefix()).append("_%04d.jpg");
    args << "--" << this->getSourcePDF();
    setOutputExtension("jpg");
    execInternal(command, args);
}

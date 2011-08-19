#include "pdf2ppt.h"
#include <QStringList>
#include <QString>

PDF2PPT::PDF2PPT() : PDFExtractor()
{
}

void PDF2PPT::exec()
{
    QString command = "pdftoppm";
    QStringList args;
    args << "-jpeg";
    if ((!getStopPage().isEmpty()) || (getStopPage().toInt() > 0)) {
        if (getStartPage().toInt() == 0)
              this->setStartPage("1");
        args << "-f" << getStartPage() << "-l" << getStopPage();
    }
    args << "-rx" << "600" << "-ry" << "600" << this->getSourcePDF();
    if (!getOutputDir().endsWith("/"))
        setOutputDir(getOutputDir().append('/'));
    setOutputPrefix(getOutputDir().append("page"));
    args << getOutputPrefix();
    setOutputExtension("jpg");
    execInternal(command, args);
}

#ifndef PDF2PPT_H
#define PDF2PPT_H

#include "pdfextractor.h"

class PDF2PPT : public PDFExtractor
{
public:
    PDF2PPT();
    virtual void exec();
};

#endif // PDF2PPT_H

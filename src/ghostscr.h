#ifndef GHOSTSCR_H
#define GHOSTSCR_H

#include "pdfextractor.h"

class GhostScr : public PDFExtractor
{
public:
    GhostScr();
    virtual void exec();
};

#endif // PDF2PPT_H

/*
    YAGF - cuneiform and tesseract OCR graphical front-end
    Copyright (C) 2011 Andrei Borovsky <anb@symmetrica.net>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "ccbuilder.h"
#include <QObject>
//#include <QRect>
#include <QHash>
#include <QMultiHash>
#include <QList>
#include <QPoint>

typedef struct _Rect
{
    qint32 x1, x2, y1, y2;
} Rect;

bool operator==(Rect r1, Rect r2);

typedef QHash<quint32, Rect> ComponentParameters;
typedef QList<qint32> Strings;
typedef QList<Rect> StringsBoxes;
typedef QMultiHash<quint32, Rect> GlyphField;
typedef QList<QPoint> TextLine;
typedef QMultiHash<quint32, TextLine> LineField;
typedef QList<TextLine> Lines;

 

class CCAnalysis : public QObject
{
public:
        CCAnalysis(CCBuilder * builder);
	~CCAnalysis();
        void analize();
        TextLine extractLine();
	int getGlyphCount();
	int getMediumGlyphHeight();
	int getMediumGlyphWidth();
	int getMediumLetterSpace();
	int getMediumWordSpace();
	int getStringsCount();
        int getGlyphBoxCount();
        Rect getGlyphBox(int index);
        QRect getStringBox(const int index) const;
        Lines getLines();
        qreal getK();
        void rotateLines(qreal phi, const QPoint &c = QPoint(0,0));
private:
        void extractComponents();
        void classifyGlyphs();
        int findAdjacent(Rect &r);
        void normalizeLines();
        void rotatePhi(qreal phi, const QPoint &c, QPoint &p);
        CCBuilder * builder;
	ComponentParameters components;
	Strings strings;
        StringsBoxes boxes;
        GlyphField glyphField;
        Lines lines;
	int glyphCount;
	int mediumGlyphHeight;
	int mediumGlyphWidth;
	int mediumLetterSpace;
	int mediumWordSpace;
	int stringsCount;
        qreal k;
};

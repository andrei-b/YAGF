#ifndef CCANALYSIS_H
#define CCANALYSIS_H

#include <QRgb>
#include "ycommon.h"

class QImage;

class CCAnalysis {
public:
    CCAnalysis(QImage * image);
    ~CCAnalysis();
    void setWhite(int minBrightness, int maxComponent);
    void setBlack(int maxBrightness, int maxComponent);
    bool findNonBlackSegment(int lineIndex);
    bool findBlackSegment(int lineIndex);
    bool findNonBlackArea(const QPoint &startPoint, bool rightwise);
    bool findBlackArea(const QPoint &startPoint, bool rightwise);
    bool findWhiteArea(const QPoint &startPoint, bool rightwise);
    bool findNonWhiteArea(const QPoint &startPoint, bool rightwise);
    bool isPointBlack(const QPoint &point);
    bool isPointWhite(const QPoint &point);
    void setMaxArea(int maxArea); // setting this to -1 requires the full area scan
    void setMaxLength(int maxLength); // when looking for vertical lines sets the maximum skew
    void setMaxHeight(int maxHeight); // when looking for horizontal  lines sets the maximum skew
    void setMinLength(int minLength);
    void setMinHeight(int minHeight);
    bool findHorzNonWhiteLine(int lineIndex);
    bool findVertNonWhiteLine(int lineIndex, int startX);
    bool findHorzBlackLine(int lineIndex);
    bool findVertBlackLine(int lineIndex, int startX);
    void setConnectionType(bool strongConnection);
    QPointList * getBorders();
private:
    QImage * m_image;
    int minWhiteBrightness;
    int maxSingleWhiteComponent;
    int maxBlackBrightness;
    int maxSingleBlackComponent;
    int currentLineIndex;
    int currentX1;
    int currentX2;
    int m_maxArea;
    int m_maxLength;
    int m_maxHeight;
    int m_minLength;
    int m_minHeight;
    bool m_strongConnection;
    QRgb ** colorField;
    QPointList * borders;
    void resetColorField();
};

#endif

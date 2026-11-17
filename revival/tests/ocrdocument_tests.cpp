#include "ocrdocument.h"

#include <QJsonArray>
#include <QJsonObject>

#include <cassert>

int main()
{
    const QByteArray tsv =
        "level\tpage_num\tblock_num\tpar_num\tline_num\tword_num\tleft\ttop\twidth\theight\tconf\ttext\n"
        "1\t1\t0\t0\t0\t0\t0\t0\t200\t100\t-1\t\n"
        "5\t1\t1\t1\t1\t1\t10\t20\t30\t12\t91.2\tHello\n"
        "5\t1\t1\t1\t1\t2\t50\t20\t40\t12\t75\tworld\n"
        "5\t1\t1\t1\t2\t1\t10\t40\t30\t12\t80\tAgain\n"
        "5\t1\t1\t1\t2\t2\t190\t40\t30\t12\t80\tOutside\n";
    OcrDocument doc = OcrDocument::fromTsv(tsv, 200, 100, QStringLiteral("scan.png"), QStringLiteral("eng"));
    assert(doc.words.size() == 3);
    assert(doc.words.at(0).bounds == QRect(10, 20, 30, 12));
    assert(doc.plainText() == QStringLiteral("Hello world\nAgain"));
    doc.words[1].text = QStringLiteral("earth");
    doc.words[1].reviewed = true;
    assert(doc.plainText() == QStringLiteral("Hello earth\nAgain"));
    const QJsonArray words = doc.toJson().value(QStringLiteral("words")).toArray();
    assert(words.at(1).toObject().value(QStringLiteral("text")) == QStringLiteral("earth"));
    assert(words.at(1).toObject().value(QStringLiteral("original_text")) == QStringLiteral("world"));
    assert(words.at(1).toObject().value(QStringLiteral("reviewed")).toBool());
}

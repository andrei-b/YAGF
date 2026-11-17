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
    assert(doc.page(1)->words.size() == 3);
    assert(doc.page(1)->words.at(0).bounds == QRect(10, 20, 30, 12));
    assert(doc.page(1)->regions.size() == 1);
    assert(doc.page(1)->regions.at(0).paragraphs.at(0).lines.size() == 2);
    assert(!doc.page(1)->words.at(0).id.isEmpty());
    assert(doc.plainText() == QStringLiteral("Hello world\nAgain"));
    doc.page(1)->words[1].text = QStringLiteral("earth");
    doc.page(1)->words[1].reviewed = true;
    assert(doc.plainText() == QStringLiteral("Hello earth\nAgain"));
    const QJsonArray words = doc.toJson().value(QStringLiteral("pages")).toArray().at(0).toObject().value(QStringLiteral("words")).toArray();
    assert(words.at(1).toObject().value(QStringLiteral("text")) == QStringLiteral("earth"));
    assert(words.at(1).toObject().value(QStringLiteral("original_text")) == QStringLiteral("world"));
    assert(words.at(1).toObject().value(QStringLiteral("reviewed")).toBool());
    const QJsonArray order = doc.toJson().value(QStringLiteral("pages")).toArray().at(0).toObject()
        .value(QStringLiteral("reading_order")).toArray();
    assert(order.size() == 3);
    assert(order.at(0).toString() == doc.page(1)->words.at(0).id);
    OcrDocument second = OcrDocument::fromTsv(tsv, 200, 100, QStringLiteral("scan.png"), QStringLiteral("eng"), 2);
    doc.totalPages = 2;
    doc.replacePage(std::move(*second.page(2)));
    assert(doc.page(2)->words.at(0).page == 2);
    assert(doc.wordCount() == 6);
    assert(doc.plainText().contains(QStringLiteral("\f")));
}

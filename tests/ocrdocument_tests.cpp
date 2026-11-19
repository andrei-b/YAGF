#include "ocrdocument.h"
#include "wordeditor.h"

#include <QJsonArray>
#include <QJsonObject>

#include <tuple>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

using namespace yagf::core;

int main()
{
    OcrDocument doc;
    doc.sourcePath = QStringLiteral("scan.png");
    doc.language = QStringLiteral("eng");
    OcrPage source;
    source.number = 1;
    source.width = source.sourceWidth = 200;
    source.height = source.sourceHeight = 100;
    source.language = QStringLiteral("eng");
    source.words.append({1, 1, 1, 1, QRect(10, 20, 30, 12),
                         QStringLiteral("Hello"), QStringLiteral("Hello"), 91.2});
    source.words.append({1, 1, 1, 1, QRect(50, 20, 40, 12),
                         QStringLiteral("world"), QStringLiteral("world"), 75});
    source.words.append({1, 1, 1, 2, QRect(10, 40, 30, 12),
                         QStringLiteral("Again"), QStringLiteral("Again"), 80});
    doc.replacePage(source);
    assert(doc.page(1)->words.size() == 3);
    assert(doc.page(1)->words.at(0).bounds == QRect(10, 20, 30, 12));
    assert(doc.page(1)->regions.size() == 1);
    assert(doc.page(1)->regions.at(0).paragraphs.at(0).lines.size() == 2);
    assert(!doc.page(1)->words.at(0).id.isEmpty());
    assert(doc.plainText() == QStringLiteral("Hello world\nAgain"));
    doc.page(1)->words[1].text = QStringLiteral("earth");
    doc.page(1)->words[1].reviewed = true;
    doc.page(1)->words[1].bold = true;
    assert(doc.plainText() == QStringLiteral("Hello earth\nAgain"));
    assert(doc.layoutHtml().contains(QStringLiteral("earth")));
    assert(doc.layoutHtml().contains(QStringLiteral("font-weight:bold")));
    assert(doc.layoutHtml().contains(QStringLiteral("data-word-id")));
    const QJsonArray words = doc.toJson().value(QStringLiteral("pages")).toArray().at(0).toObject().value(QStringLiteral("words")).toArray();
    assert(words.at(1).toObject().value(QStringLiteral("text")) == QStringLiteral("earth"));
    assert(words.at(1).toObject().value(QStringLiteral("original_text")) == QStringLiteral("world"));
    assert(words.at(1).toObject().value(QStringLiteral("reviewed")).toBool());
    const QJsonArray order = doc.toJson().value(QStringLiteral("pages")).toArray().at(0).toObject()
        .value(QStringLiteral("reading_order")).toArray();
    assert(order.size() == 3);
    assert(order.at(0).toString() == doc.page(1)->words.at(0).id);
    OcrPage second = source;
    second.number = 2;
    second.id.clear();
    for (OcrWord &word : second.words) { word.page = 2; word.id.clear(); word.sourceWordIds.clear(); }
    doc.totalPages = 2;
    doc.replacePage(std::move(second));
    assert(doc.page(2)->words.at(0).page == 2);
    assert(doc.wordCount() == 6);
    assert(doc.plainText().contains(QStringLiteral("\f")));

    OcrDocument layout;
    layout.sourcePath = QStringLiteral("scan.png");
    OcrPage layoutPage;
    layoutPage.number = 1;
    layoutPage.width = layoutPage.sourceWidth = 200;
    layoutPage.height = layoutPage.sourceHeight = 100;
    layoutPage.words.append({1, 1, 1, 1, QRect(10, 20, 30, 12),
                             QStringLiteral("Left"), QStringLiteral("Left"), 90});
    layoutPage.words.append({1, 2, 1, 1, QRect(110, 20, 40, 12),
                             QStringLiteral("Right"), QStringLiteral("Right"), 90});
    layout.replacePage(std::move(layoutPage));
    assert(layout.page(1)->regions.size() == 2);
    layout.page(1)->regions[1].type = QStringLiteral("column");
    layout.page(1)->regions[1].bounds = QRect(100, 10, 70, 50);
    assert(layout.page(1)->moveRegion(1, -1));
    assert(layout.plainText() == QStringLiteral("Right\n\nLeft"));
    assert(!layout.page(1)->moveRegion(0, -1));
    OcrDocument restored;
    QString error;
    assert(OcrDocument::fromJson(layout.toJson(), &restored, &error));
    assert(restored.plainText() == layout.plainText());
    assert(restored.page(1)->regions.at(0).type == QStringLiteral("column"));
    assert(restored.page(1)->regions.at(0).bounds == QRect(100, 10, 70, 50));
    assert(!layout.useRawOcrOutput && !restored.useRawOcrOutput);
    auto rawMode = layout.toJson();
    rawMode.insert("use_raw_ocr_output", true);
    assert(OcrDocument::fromJson(rawMode, &restored, &error) && restored.useRawOcrOutput);
    assert(restored.toJson().value("use_raw_ocr_output").toBool());
    rawMode.remove("use_raw_ocr_output");
    assert(OcrDocument::fromJson(rawMode, &restored, &error) && !restored.useRawOcrOutput);
    rawMode.insert("use_raw_ocr_output", "yes");
    assert(!OcrDocument::fromJson(rawMode, &restored, &error));

    OcrDocument manual;
    manual.sourcePath = QStringLiteral("image.png");
    OcrPage manualPage;
    manualPage.number = 1;
    manualPage.width = manualPage.sourceWidth = 200;
    manualPage.height = manualPage.sourceHeight = 100;
    manualPage.imageRegions = {{QStringLiteral("area-a"), QRect(10, 10, 50, 40)},
                               {QStringLiteral("area-b"), QRect(100, 20, 60, 50)}};
    manual.replacePage(manualPage);
    assert(OcrDocument::fromJson(manual.toJson(), &restored, &error));
    assert(restored.page(1)->words.isEmpty());
    assert(restored.page(1)->imageRegions.size() == 2);
    assert(restored.page(1)->imageRegions.at(1).bounds == QRect(100, 20, 60, 50));
    QJsonObject invalidArea = manual.toJson();
    QJsonArray areaPages = invalidArea.value(QStringLiteral("pages")).toArray();
    QJsonObject areaPage = areaPages.at(0).toObject();
    QJsonArray areas = areaPage.value(QStringLiteral("image_regions")).toArray();
    QJsonObject area = areas.at(0).toObject();
    area.insert(QStringLiteral("box"), QJsonObject{{"x", 195}, {"y", 10},
                                                    {"width", 50}, {"height", 40}});
    areas.replace(0, area);
    areaPage.insert(QStringLiteral("image_regions"), areas);
    areaPages.replace(0, areaPage);
    invalidArea.insert(QStringLiteral("pages"), areaPages);
    assert(!OcrDocument::fromJson(invalidArea, &restored, &error));

    OcrPage extracted = *doc.page(1);
    assert(!extracted.extractRegion({0, 1, 2}));
    assert(extracted.extractRegion({1}));
    assert(extracted.regions.size() == 2);
    assert(extracted.words.last().text == QStringLiteral("earth"));
    assert(extracted.words.last().reviewed);
    assert(extracted.regions.last().bounds == QRect(50, 20, 40, 12));

    const auto split = WordEditor::apply(*doc.page(1), {0},
        {{QStringLiteral("Hel")}, {QStringLiteral("lo")}});
    assert(split && split->words.size() == 4);
    assert(split->words.at(0).text == QStringLiteral("Hel"));
    assert(split->words.at(1).text == QStringLiteral("lo"));
    assert(split->words.at(0).id != split->words.at(1).id);
    assert(split->words.at(0).sourceWordIds == split->words.at(1).sourceWordIds);
    assert(split->words.at(0).bounds.united(split->words.at(1).bounds) == doc.page(1)->words.at(0).bounds);
    assert(split->words.at(0).sourceBoxes.size() == 1 && split->words.at(1).sourceBoxes.size() == 1);
    const auto joined = WordEditor::apply(*split, {0, 1}, {{QStringLiteral("Hello")}});
    assert(joined && joined->words.size() == 3);
    assert(joined->words.at(0).bounds == doc.page(1)->words.at(0).bounds);
    const auto inserted = WordEditor::apply(*joined, {0, 1},
        {{QStringLiteral("Hello")}, {QStringLiteral("new")}, {QStringLiteral("earth")}});
    assert(inserted && inserted->words.size() == 4);
    assert(inserted->words.at(1).originalText.isEmpty());
    assert(inserted->words.at(1).sourceWordIds.isEmpty());
    assert(inserted->words.at(1).sourceBoxes.isEmpty());
    assert(inserted->words.at(1).bounds.isValid());
    const auto deleted = WordEditor::apply(*inserted, {1}, {});
    assert(deleted && deleted->words.size() == 3);
    assert(deleted->words.at(1).text == QStringLiteral("earth"));
    const auto merged = WordEditor::apply(*doc.page(1), {0, 1}, {{QStringLiteral("Helloearth")}});
    assert(merged && merged->words.at(0).sourceWordIds.size() == 2);
    assert(merged->words.at(0).sourceBoxes.size() == 2);
    OcrDocument editedDocument = doc;
    editedDocument.replacePage(*merged);
    OcrDocument reloaded;
    assert(OcrDocument::fromJson(editedDocument.toJson(), &reloaded, &error));
    assert(reloaded.page(1)->words.at(0).sourceWordIds == merged->words.at(0).sourceWordIds);
    assert(reloaded.page(1)->words.at(0).sourceBoxes == merged->words.at(0).sourceBoxes);
    QJsonObject brokenGeometry = editedDocument.toJson();
    QJsonArray pages = brokenGeometry.value(QStringLiteral("pages")).toArray();
    QJsonObject firstPage = pages.at(0).toObject();
    QJsonArray corruptWords = firstPage.value(QStringLiteral("words")).toArray();
    QJsonObject firstWord = corruptWords.at(0).toObject();
    firstWord.insert(QStringLiteral("source_boxes"), QJsonArray{QJsonObject{
        {"x", 199}, {"y", 20}, {"width", 1}, {"height", 1}}});
    corruptWords.replace(0, firstWord);
    firstPage.insert(QStringLiteral("words"), corruptWords);
    pages.replace(0, firstPage);
    brokenGeometry.insert(QStringLiteral("pages"), pages);
    assert(!OcrDocument::fromJson(brokenGeometry, &reloaded, &error));
    editedDocument.replacePage(*inserted);
    assert(OcrDocument::fromJson(editedDocument.toJson(), &reloaded, &error));
    assert(reloaded.page(1)->words.at(1).sourceWordIds.isEmpty());
    assert(!WordEditor::apply(*doc.page(1), {0, 2}, {{QStringLiteral("bad")}}));

    OcrPage phrase;
    phrase.number = 1;
    phrase.width = 300;
    phrase.height = 80;
    for (const auto &[text, x, width] :
         {std::tuple{QStringLiteral("mod"), 10, 30},
          std::tuple{QStringLiteral("ernization"), 41, 80},
          std::tuple{QStringLiteral("and"), 130, 30},
          std::tuple{QStringLiteral("ofthe"), 170, 50}})
        phrase.words.append({1, 1, 1, 1, QRect(x, 20, width, 12), text, text, 90});
    phrase.rebuildHierarchy();
    const auto multiple = WordEditor::apply(phrase, {0, 1, 2, 3},
        {{QStringLiteral("modernization")}, {QStringLiteral("and")},
         {QStringLiteral("of")}, {QStringLiteral("the")}});
    assert(multiple && multiple->words.size() == 4);
    assert(multiple->words.at(1).bounds == phrase.words.at(2).bounds);
    assert(multiple->words.at(0).sourceWordIds.size() == 2);
    assert(multiple->words.at(2).sourceWordIds == multiple->words.at(3).sourceWordIds);
    assert(multiple->words.at(2).sourceBoxes.first().united(
        multiple->words.at(3).sourceBoxes.first()) == phrase.words.at(3).bounds);

    OcrPage hebrew;
    hebrew.width = 200;
    hebrew.height = 40;
    const QString combined = QStringLiteral("יוםטוב");
    const QString next = QStringLiteral("לכולם");
    hebrew.words.append({1, 1, 1, 1, QRect(100, 10, 60, 15), combined, combined, 90});
    hebrew.words.append({1, 1, 1, 1, QRect(20, 10, 40, 15), next, next, 90});
    hebrew.rebuildHierarchy();
    const auto rtlSplit = WordEditor::apply(hebrew, {0},
        {{QStringLiteral("יום")}, {QStringLiteral("טוב")}});
    assert(rtlSplit && rtlSplit->words[0].sourceBoxes == QVector<QRect>{QRect(130, 10, 30, 15)});
    assert(rtlSplit->words[1].sourceBoxes == QVector<QRect>{QRect(100, 10, 30, 15)});
    const auto rtlInsert = WordEditor::apply(hebrew, {0, 1},
        {{QStringLiteral("א")}, {combined}, {QStringLiteral("ב")}, {QStringLiteral("ג")},
         {next}, {QStringLiteral("ד")}});
    assert(rtlInsert && rtlInsert->words.size() == 6);
    assert(rtlInsert->words[0].bounds == QRect(160, 10, 40, 15));
    assert(rtlInsert->words[2].bounds == QRect(80, 10, 20, 15));
    assert(rtlInsert->words[3].bounds == QRect(60, 10, 20, 15));
    assert(rtlInsert->words[5].bounds == QRect(0, 10, 20, 15));

    OcrPage acrossLines;
    acrossLines.number = 1;
    acrossLines.width = 250;
    acrossLines.height = 100;
    acrossLines.words.append({1, 1, 1, 1, QRect(10, 10, 25, 12),
                              QStringLiteral("hyphen-"), QStringLiteral("hyphen-"), 90});
    acrossLines.words.append({1, 1, 1, 2, QRect(160, 70, 40, 12),
                              QStringLiteral("ated"), QStringLiteral("ated"), 90});
    acrossLines.rebuildHierarchy();
    const auto joinedLines = WordEditor::apply(acrossLines, {0, 1}, {{QStringLiteral("hyphenated")}});
    assert(joinedLines && joinedLines->words.size() == 1);
    assert(joinedLines->words.first().sourceBoxes ==
           QVector<QRect>({QRect(10, 10, 25, 12), QRect(160, 70, 40, 12)}));
    assert(joinedLines->words.first().bounds == QRect(10, 10, 190, 72));

    OcrPage longPage;
    longPage.number = 1;
    longPage.width = 3200;
    longPage.height = 20;
    QVector<int> longSelection;
    QVector<EditedWord> longReplacement;
    for (int i = 0; i < 1500; ++i) {
        const QString word = QStringLiteral("word%1").arg(i);
        longPage.words.append({1, 1, 1, 1, QRect(i * 2, 0, 1, 10), word, word, 90});
        longSelection.append(i);
        longReplacement.append({word});
    }
    longPage.rebuildHierarchy();
    longReplacement.last().text = QStringLiteral("lastpart");
    longReplacement.append({QStringLiteral("two")});
    const auto longEdit = WordEditor::apply(longPage, longSelection, longReplacement);
    assert(longEdit && longEdit->words.size() == 1501);
    assert(longEdit->words.at(0).bounds == longPage.words.at(0).bounds);
}

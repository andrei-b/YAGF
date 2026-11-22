#include "verificationcontroller.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QListView>
#include <QLineEdit>
#include <QMainWindow>
#include <QPushButton>
#include <QtTest>
using namespace yagf;
class VerificationControllerTests : public QObject {
    Q_OBJECT
private slots:
    void sharedWordRequiresInkInBothBlockShapes() {
        core::OcrDocument book;
        core::OcrPage page; page.engineId="test"; page.width=page.height=100;
        const QRect top(0,0,100,30), lower(0,30,100,30), tall(16,30,7,12);
        core::ImageRegion a{"upper"},b{"lower"};
        a.setShape(QRegion(top)|tall); b.setShape(QRegion(lower)-tall);
        page.imageRegions={a,b};
        page.words={{1,1,1,1,tall,"p","p",95},{1,2,1,1,{60,30,10,9},"z","z",95}};
        book.replacePage(page);
        QVERIFY(core::buildVerificationQueue(book).problemBlocks.isEmpty());
        book.pages[0].imageRegions[1].setShape(lower);
        const auto problems=core::buildVerificationQueue(book).problemBlocks;
        QCOMPARE(problems.size(),2);
        for (const auto &p:problems) QCOMPARE(p.reasons,unsigned(core::SharedWord));
    }
    void formulasBlocksAndWordsAcrossPages() {
        core::OcrDocument book; book.sourcePath="book.pdf"; book.totalPages=3;
        core::OcrPage first; first.number=1; first.engineId="test"; first.language="no_dictionary";
        first.width=first.height=first.sourceWidth=first.sourceHeight=150;
        first.words={{1,1,1,1,{5,5,110,12},"Two: $x$ and $y$.","Two: $x$ and $y$.",96,true},
                     {1,2,1,1,{8,65,30,12},"weak","weak",32}};
        core::ImageRegion block{"line",{2,54,100,40}};
        block.setShape(QRegion(2,60,100,34)|QRect(4,54,10,6));
        first.imageRegions={block,{"empty",{110,60,30,20},{},false,"math"},
                            {"photo",{110,95,30,20},{},true,"picture"}};
        book.replacePage(first);
        auto second=first; second.number=2; second.imageRegions.clear(); second.words.removeFirst();
        book.replacePage(second);
        core::OcrPage third; third.number=3; third.imageRegions={{"unread",{5,5,40,20},{},false,"math"}};
        book.replacePage(third);
        QMainWindow window; ui::VerificationController controller(&window,book);
        QVector<core::VerificationItem> jumps;
        connect(&controller,&ui::VerificationController::activateRequested,this,[&](const auto &item){jumps<<item;});
        window.show(); controller.show();
        auto *list=window.findChild<QListView *>("verificationItems");
        auto *search=window.findChild<QLineEdit *>("navigatorSearch");
        const auto mode=[&](int i){window.findChild<QPushButton *>("navigatorCategory"+QString::number(i))->click();};
        mode(1); QCOMPARE(list->model()->rowCount(),4);
        controller.navigate(1); QCOMPARE(jumps.last().text,"x"); QVERIFY(jumps.last().reviewed);
        controller.navigate(1); QCOMPARE(jumps.last().text,"y"); QCOMPARE(jumps.last().formulaStart,14);
        controller.navigate(-1); QCOMPARE(jumps.last().text,"x");
        controller.navigate(-1); QCOMPARE(jumps.last().blockId,"unread");
        mode(2); QCOMPARE(list->model()->rowCount(),3);
        search->setText("No OCR text"); QCOMPARE(list->model()->rowCount(),1);
        controller.navigate(1); QCOMPARE(jumps.last().blockId,"empty");
        search->clear(); controller.setPage(2); controller.navigate(1);
        QCOMPARE(jumps.last().page,2); QCOMPARE(jumps.last().target,core::NavigationTarget::OcrBlock);
        mode(3); QCOMPARE(list->model()->rowCount(),2);
        controller.navigate(1); QCOMPARE(jumps.last().page,2); QCOMPARE(jumps.last().text,"weak");
        book.pages[1].words[0].reviewed=true; controller.invalidate(2);
        QTRY_COMPARE(list->model()->rowCount(),1);
        controller.navigate(1); QCOMPARE(jumps.last().page,1);
    }
    void navigationFiltersAndActions() {
        core::OcrDocument document; document.sourcePath = "book.pdf"; document.totalPages = 3;
        for (int number : {1, 2}) {
            core::OcrPage page; page.number = number; page.engineId = "test"; page.language = "no_dictionary";
            page.width = page.sourceWidth = page.height = page.sourceHeight = 100;
            core::OcrWord word; word.id = QString::number(number); word.text = word.originalText = "hello";
            word.page = number; word.bounds = {10,10,20,10}; word.confidence = 20;
            page.words = {word}; document.replacePage(page);
        }
        auto pending = document.pages[0].words[0]; pending.id = "pending"; pending.reviewed = true;
        document.pages[0].pendingCorrections = {pending};
        QMainWindow window; ui::VerificationController controller(&window, document);
        QVector<core::VerificationItem> activated, verified, reviewed, discarded;
        connect(&controller, &ui::VerificationController::activateRequested, this, [&](const auto &item) { activated.append(item); });
        connect(&controller, &ui::VerificationController::verifyRequested, this, [&](const auto &item) { verified.append(item); });
        connect(&controller, &ui::VerificationController::reviewRequested, this, [&](const auto &item) { reviewed.append(item); });
        connect(&controller, &ui::VerificationController::discardRequested, this, [&](const auto &item) { discarded.append(item); });
        window.show(); controller.show();
        auto *list = window.findChild<QListView *>("verificationItems");
        auto *filter = window.findChild<QComboBox *>("verificationFilter");
        auto *pageOnly = window.findChild<QCheckBox *>();
        QVERIFY(list && filter && pageOnly); QCOMPARE(list->model()->rowCount(), 4);
        controller.navigate(1); QCOMPARE(activated.last().page, 1); QVERIFY(!activated.last().pending);
        controller.navigate(1); QVERIFY(activated.last().pending);
        controller.navigate(1); QCOMPARE(activated.last().page, 2);
        controller.navigate(-1); QVERIFY(activated.last().pending);
        filter->setCurrentIndex(filter->findData(core::LowConfidence)); QCOMPARE(list->model()->rowCount(), 2);
        pageOnly->setChecked(true); QCOMPARE(list->model()->rowCount(), 1);
        controller.invalidate(2); QTRY_COMPARE(list->model()->index(0,0).data().toString().startsWith("Page 2"), true);
        const int before = activated.size(); controller.setBusy(true); controller.navigate(1); QCOMPARE(activated.size(), before);
        controller.setBusy(false); controller.navigate(1); QCOMPARE(activated.last().page, 2);
        filter->setCurrentIndex(filter->findData(core::PendingCorrection)); pageOnly->setChecked(false);
        QCOMPARE(list->model()->rowCount(),1); controller.navigate(1);
        for (auto *button : window.findChildren<QPushButton *>()) {
            if (button->text() == "Mark verified") button->click();
            if (button->text() == "Review / restore…") button->click();
            if (button->text() == "Discard saved correction…") button->click();
        }
        QVERIFY(verified.isEmpty()); QCOMPARE(reviewed.size(),1); QCOMPARE(discarded.size(),1); QVERIFY(discarded[0].pending);
        document.pages[0].pendingCorrections.clear(); controller.invalidate(2); QTRY_COMPARE(list->model()->rowCount(), 0);
    }
};
QTEST_MAIN(VerificationControllerTests)
#include "verificationcontroller_tests.moc"

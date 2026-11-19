#include "verificationcontroller.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QListView>
#include <QMainWindow>
#include <QPushButton>
#include <QtTest>
using namespace yagf;
class VerificationControllerTests : public QObject {
    Q_OBJECT
private slots:
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

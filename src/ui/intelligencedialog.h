#pragma once
#include "documentintelligence.h"
#include "ocrservices.h"
#include <QDialog>

class QLabel;
class QListWidget;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace yagf::ui {
class IntelligenceDialog final : public QDialog {
public:
    static constexpr int ReviewWithAi = 2;
    IntelligenceDialog(const core::OcrDocument& document, int number, QImage image,
        const QVector<processing::OcrService>& services, const QString& areaId, int selectedWord,
        QWidget* parent);
    std::optional<core::OcrPage> edited;
    std::optional<processing::OcrService> requestedService;
    intelligence::Assessment assessment;
    int word = -1;
    QVector<int> aiWords;

private:
    void selectNode(int node);
    intelligence::DocumentIntelligence engine;
    core::OcrPage page;
    QImage scan;
    QVector<processing::OcrService> services;
    std::optional<processing::OcrService> compatible;
    QVector<QTreeWidgetItem*> items;
    QTreeWidget* tree;
    QListWidget *links, *choices;
    QLabel *crop, *message;
    QPushButton *use, *request, *confirm;
};
}

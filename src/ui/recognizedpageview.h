#pragma once

#include <QTextEdit>
#include <QImage>
#include <atomic>
#include <memory>

namespace yagf::core { struct OcrPage; }

namespace yagf::ui {

class RecognizedPageView final : public QTextEdit {
public:
    explicit RecognizedPageView(QWidget *parent = nullptr);
    ~RecognizedPageView() override;
    void setPage(const core::OcrPage *page);

protected:
    QMimeData *createMimeDataFromSelection() const override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void sizeFormulaImages();
    QVector<QImage> formulaImages;
    std::shared_ptr<std::atomic_bool> cancellation;
};

}

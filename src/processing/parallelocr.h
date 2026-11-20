#pragma once
#include "ocrservices.h"
#include <future>
#include <vector>

namespace yagf::processing {
struct ComparisonEngine {OcrService service; std::unique_ptr<IOcrEngine> engine;};
// Starts companions immediately; destruction cancels and joins outstanding work.
class ParallelOcrRun {
public:
    ParallelOcrRun(std::vector<ComparisonEngine> &engines,const QImage &image,const OcrOptions &options,
                   const QVector<QRect> &areas,std::stop_token parent);
    ~ParallelOcrRun();
    QVector<core::OcrReading> finish();
private:
    std::stop_source stop;
    std::stop_callback<std::function<void()>> parentStop;
    std::vector<std::future<core::OcrReading>> futures;
};
}

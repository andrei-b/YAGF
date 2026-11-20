#pragma once
#include "nativetraining.h"
#include <atomic>
namespace yagf::processing {
struct NativeTrainingDataset {
    QString sourceId;
    QVector<NativeTrainingSample> samples;
    int skipped=0;
};
std::expected<NativeTrainingDataset,QString> collectNativeTrainingSamples(const core::OcrDocument &book,int first,int last,
    const std::atomic_bool &cancelled,bool includeSymbols=true);
std::expected<void,QString> writeNativeTrainingBundle(const QString &directory,const NativeTrainingDataset &dataset,
    const std::atomic_bool &cancelled);
QString nativeTrainingSourceId(const QString &path);
}

#include "parallelocr.h"
#include "tesseractengine.h"
#include "recognitionrecorder.h"
#include <QSet>
#include <QHash>
#include <exception>
#include <stdexcept>

namespace yagf::processing {
ParallelOcrRun::ParallelOcrRun(std::vector<ComparisonEngine> &engines,const QImage &image,const OcrOptions &options,
    const QVector<QRegion> &areas,std::stop_token parent,const QString &serviceKey,bool includeService):parentStop(parent,[this]{stop.request_stop();}) {
    QSet<QString> seen;
    for (auto &entry:engines) {
        if (!serviceKey.isEmpty() && (entry.service.key()==serviceKey)!=includeService) continue;
        if (futures.size()>=3 || !entry.engine || seen.contains(entry.service.key())) continue;
        seen.insert(entry.service.key()); const auto token=stop.get_token();
        futures.emplace_back(std::async(std::launch::async,[engine=entry.engine.get(),service=entry.service,image,options,areas,token] {
            core::OcrReading reading; reading.source=service.key(); reading.label=service.label; reading.engine=engine->id(); reading.language=service.language;
            OcrOptions settings=options; settings.progress={}; settings.historyRole="companion"; settings.historySource=service.key();
            if (service.provider=="tesseract") settings.language=TesseractEngine::modelLanguage(service.language.isEmpty() ? options.language : service.language);
            else settings.language=service.language.isEmpty() ? QStringLiteral("auto") : service.language;
            qsizetype characters=0; int nextBlock=1;
            try {
                for (const auto &shape:areas) {
                    if (token.stop_requested()) break;
                    core::ImageRegion block; block.setShape(shape);
                    const auto area = block.bounds;
                    reading.area|=area;
                    auto result=engine->recognize(block.crop(image),croppedOcrOptions(settings,area),token);
                    if (!result) {reading.error=result.error().cancelled ? QStringLiteral("Cancelled") : result.error().message.left(2048); reading.items.clear(); break;}
                    if (result->number!=options.pageNumber || result->width!=area.width() || result->height!=area.height()) throw std::runtime_error("Companion OCR returned inconsistent page dimensions");
                    reading.language=result->language;
                    for (auto evidence : result->recognitionEvidence) {
                        auto &areaInPage = evidence.provenance.inputArea;
                        if (areaInPage.isEmpty()) areaInPage = QRect(QPoint(), evidence.provenance.imageSize);
                        areaInPage.translate(area.topLeft()); reading.recognitionEvidence.append(std::move(evidence));
                    }
                    QHash<int,int> blocks;
                    for (const auto &w:result->words) {
                        if (!QRect(QPoint{},area.size()).contains(w.bounds) || w.bounds.isEmpty() || w.text.isEmpty() || w.text.size()>65536 || w.block<0 || w.paragraph<0 || w.line<0 || w.paragraph>1000000 || w.line>1000000) throw std::runtime_error("Companion OCR returned invalid text or geometry");
                        characters+=w.text.size(); if (characters>1024*1024 || reading.items.size()>=10000) throw std::runtime_error("Companion OCR output exceeds the limit");
                        if (!blocks.contains(w.block)) blocks.insert(w.block,nextBlock++);
                        reading.items.append({w.bounds.translated(area.topLeft()),w.text,blocks[w.block],w.paragraph,w.line,w.layoutBlock,w.geometryInferred,result->recognitionHistory.runs.isEmpty()?QString():result->recognitionHistory.runs.last().id});
                    }
                }
            } catch (const std::exception &error) {reading.error=QString::fromUtf8(error.what()).left(2048); reading.items.clear();}
            if (reading.area.isEmpty()) reading.area=image.rect();
            if(options.history) for(const auto &run:options.history->snapshot().runs)
                if(run.source==reading.source && run.role=="companion")reading.historyIds.append(run.id);
            return reading;
        }));
    }
}
ParallelOcrRun::~ParallelOcrRun() {stop.request_stop(); for (auto &f:futures) if (f.valid()) f.wait();}
QVector<core::OcrReading> ParallelOcrRun::finish() {
    QVector<core::OcrReading> result; for (auto &f:futures) if (f.valid()) result.append(f.get()); return result;
}
}

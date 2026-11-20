#include "parallelocr.h"
#include "tesseractengine.h"
#include <QSet>
#include <QHash>
#include <exception>
#include <stdexcept>

namespace yagf::processing {
ParallelOcrRun::ParallelOcrRun(std::vector<ComparisonEngine> &engines,const QImage &image,const OcrOptions &options,
    const QVector<QRect> &areas,std::stop_token parent):parentStop(parent,[this]{stop.request_stop();}) {
    QSet<QString> seen;
    for (auto &entry:engines) {
        if (futures.size()>=3 || !entry.engine || seen.contains(entry.service.key())) continue;
        seen.insert(entry.service.key()); const auto token=stop.get_token();
        futures.emplace_back(std::async(std::launch::async,[engine=entry.engine.get(),service=entry.service,image,options,areas,token] {
            core::OcrReading reading; reading.source=service.key(); reading.label=service.label; reading.engine=engine->id(); reading.language=service.language;
            OcrOptions settings=options; settings.progress={};
            if (service.provider=="tesseract") settings.language=TesseractEngine::modelLanguage(service.language.isEmpty() ? options.language : service.language);
            else settings.language=service.language.isEmpty() ? QStringLiteral("auto") : service.language;
            qsizetype characters=0; int nextBlock=1;
            try {
                for (const auto &area:areas) {
                    if (token.stop_requested()) break;
                    reading.area|=area;
                    auto result=engine->recognize(image.copy(area),settings,token);
                    if (!result) {reading.error=result.error().cancelled ? QStringLiteral("Cancelled") : result.error().message.left(2048); reading.items.clear(); break;}
                    if (result->number!=options.pageNumber || result->width!=area.width() || result->height!=area.height()) throw std::runtime_error("Companion OCR returned inconsistent page dimensions");
                    reading.language=result->language;
                    QHash<int,int> blocks;
                    for (const auto &w:result->words) {
                        if (!QRect(QPoint{},area.size()).contains(w.bounds) || w.bounds.isEmpty() || w.text.isEmpty() || w.text.size()>65536 || w.block<0 || w.paragraph<0 || w.line<0 || w.paragraph>1000000 || w.line>1000000) throw std::runtime_error("Companion OCR returned invalid text or geometry");
                        characters+=w.text.size(); if (characters>1024*1024 || reading.items.size()>=10000) throw std::runtime_error("Companion OCR output exceeds the limit");
                        if (!blocks.contains(w.block)) blocks.insert(w.block,nextBlock++);
                        reading.items.append({w.bounds.translated(area.topLeft()),w.text,blocks[w.block],w.paragraph,w.line,w.layoutBlock});
                    }
                }
            } catch (const std::exception &error) {reading.error=QString::fromUtf8(error.what()).left(2048); reading.items.clear();}
            if (reading.area.isEmpty()) reading.area=image.rect();
            return reading;
        }));
    }
}
ParallelOcrRun::~ParallelOcrRun() {stop.request_stop(); for (auto &f:futures) if (f.valid()) f.wait();}
QVector<core::OcrReading> ParallelOcrRun::finish() {
    QVector<core::OcrReading> result; for (auto &f:futures) if (f.valid()) result.append(f.get()); return result;
}
}

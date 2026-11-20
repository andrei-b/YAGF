#include "ocrtrainingdataset.h"
#include "exportpage.h"
#include "formulatext.h"
#include "tesseractengine.h"
#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSet>
#include <QTemporaryDir>
#include <algorithm>
#include <map>
#include <tuple>
namespace yagf::processing {
QString nativeTrainingSourceId(const QString &path)
{
    QFile file(path); if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256); if (!hash.addData(&file)) return {};
    return QString::fromLatin1(hash.result().toHex());
}
namespace {
QString sampleId(const QByteArray &png,int page,const QRect &bounds) {
    return QString::fromLatin1(QCryptographicHash::hash(png+QByteArray::number(page)+QJsonDocument(QJsonObject{{"x",bounds.x()},{"y",bounds.y()}}).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());
}
bool sameLine(const core::OcrWord &word) {
    if (word.sourceBoxes.isEmpty()) return false;
    for (const auto &box:word.sourceBoxes) if (std::abs(box.center().y()-word.sourceBoxes.first().center().y())>std::min(box.height(),word.sourceBoxes.first().height())/2) return false;
    return true;
}
bool literal(const QString &text) {return !text.isEmpty() && !text.contains('\n') && !text.contains('\t') && core::mathTextSpans(text).isEmpty();}
}
std::expected<NativeTrainingDataset,QString> collectNativeTrainingSamples(const core::OcrDocument &book,int first,int last,const std::atomic_bool &cancelled,bool includeSymbols)
{
    NativeTrainingDataset result; result.sourceId=nativeTrainingSourceId(book.sourcePath);
    if (result.sourceId.isEmpty()) return std::unexpected(QStringLiteral("Could not read the source document."));
    QTemporaryDir temporary; if (!temporary.isValid()) return std::unexpected(QStringLiteral("Could not prepare training images."));
    QSet<QString> ids; qint64 bytes=0;
    const auto append=[&](int page,QRect bounds,QString text,const QImage &image,bool reviewed,QString decision="pending") {
        if (!image.rect().contains(bounds) || bounds.isEmpty() || !literal(text) || text.size()>4096 || bounds.width()>16000 || bounds.height()>4000 || qint64(bounds.width())*bounds.height()>8000000) {++result.skipped; return;}
        QByteArray png; QBuffer buffer(&png); buffer.open(QIODevice::WriteOnly); if (!image.copy(bounds).save(&buffer,"PNG")) {++result.skipped; return;}
        const auto id=sampleId(png,page,bounds); if (ids.contains(id)) return;
        if (png.size()>2*1024*1024 || bytes+png.size()>32*1024*1024 || result.samples.size()>=5000) {++result.skipped; return;}
        ids.insert(id); bytes+=png.size();
        result.samples.append({id,page,bounds,text.normalized(QString::NormalizationForm_C).trimmed(),png,decision,"train",reviewed});
    };
    for (const auto &page:book.pages) {
        if (page.number<first || page.number>last) continue;
        if (cancelled.load()) return std::unexpected(QStringLiteral("Cancelled"));
        auto image=readExportPage(book,page.number,temporary,cancelled); if (!image) return std::unexpected(image.error().error);
        QSet<QString> excluded;
        for (const auto &region:page.regions) if (region.type=="equation" || region.type=="formula" || region.type=="table" || region.isFigure())
            for (const auto &p:region.paragraphs) for (const auto &line:p.lines) for (const auto &id:line.wordIds) excluded.insert(id);
        std::map<std::tuple<int,int,int>,QVector<const core::OcrWord *>> lines;
        for (const auto &word:page.words) lines[{word.block,word.paragraph,word.line}].append(&word);
        for (const auto &[key,words]:lines) {
            if (cancelled.load()) return std::unexpected(QStringLiteral("Cancelled"));
            QRect box; QStringList text; bool reviewed=true,valid=true;
            for (const auto *word:words) {
                if (excluded.contains(word->id) || !word->latexText.isEmpty() || !literal(word->text) || !sameLine(*word)) valid=false;
                box|=word->bounds; text.append(word->text); reviewed&=word->reviewed;
            }
            if (std::ranges::any_of(page.imageRegions,[&](const auto &region){return (region.notText || region.content=="math") && region.bounds.intersects(box);})) valid=false;
            if (!valid) {++result.skipped; continue;}
            append(page.number,box,text.join(' '),*image,reviewed);
        }
        // Paragraph-only OCR cannot supply line crops. Recover geometry locally,
        // pair it only when the saved paragraph and detected line counts agree,
        // and require fresh approval of every proposed pair.
        for (const auto &word:page.words) if (word.layoutBlock && word.text.contains('\n') && !excluded.contains(word.id) && word.latexText.isEmpty() && core::mathTextSpans(word.text).isEmpty()) {
            const auto text=word.text.split('\n',Qt::SkipEmptyParts); if (text.size()<2 || text.size()>128 || !image->rect().contains(word.bounds)) continue;
            TesseractEngine engine; std::stop_source stop;
            auto layout=engine.recognize(image->copy(word.bounds),{.language=TesseractEngine::modelLanguage(page.language),.pageNumber=page.number},stop.get_token(),&cancelled);
            if (!layout) continue;
            QVector<QRect> boxes;
            for (const auto &region:layout->regions) for (const auto &p:region.paragraphs) for (const auto &line:p.lines) boxes.append(line.bounds.translated(word.bounds.topLeft()));
            if (boxes.size()!=text.size()) continue;
            for (int i=0;i<text.size();++i) append(page.number,boxes[i],text[i].trimmed(),*image,false);
        }
    }
    if (includeSymbols) for (const auto &rule:book.symbolRules) for (const auto &e:rule.examples)
        if (e.decision=="positive" && e.page>=first && e.page<=last && !e.png.isEmpty()) {
            QImage image; if (image.loadFromData(e.png,"PNG")) append(e.page,image.rect(),rule.label,image,true,"approved");
        }
    // Keep all samples from a physical page in the same partition. Single-page
    // collections use a deterministic image-hash split and say so in the UI.
    QSet<int> pages; for (const auto &s:result.samples) pages.insert(s.page);
    auto numbers=pages.values(); std::ranges::sort(numbers);
    for (int i=0;i<result.samples.size();++i) {
        auto &s=result.samples[i]; const int index=numbers.indexOf(s.page);
        s.partition=numbers.size()>1 ? ((index%5==4 || index==numbers.size()-1) ? "validation" : "train") : (i%5==4 ? "validation" : "train");
    }
    if (result.samples.size()>=3 && std::ranges::none_of(result.samples,[](const auto &s){return s.partition=="validation";})) {
        auto choice=std::ranges::find_if(result.samples.rbegin(),result.samples.rend(),[](const auto &s){return !s.previouslyReviewed;});
        if (choice!=result.samples.rend()) choice->partition="validation";
        else result.samples.last().partition="validation";
    }
    return result;
}
std::expected<void,QString> writeNativeTrainingBundle(const QString &directory,const NativeTrainingDataset &dataset,const std::atomic_bool &cancelled)
{
    QDir dir(directory); if (!dir.exists() || !dir.entryList(QDir::AllEntries|QDir::NoDotAndDotDot).isEmpty()) return std::unexpected(QStringLiteral("Choose an empty folder for the training bundle."));
    if (std::ranges::none_of(dataset.samples,[](const auto &s){return s.decision=="approved";})) return std::unexpected(QStringLiteral("Approve examples before exporting."));
    const auto write=[&](const QString &name,const QByteArray &bytes)->bool {QSaveFile file(dir.filePath(name)); return file.open(QIODevice::WriteOnly) && file.write(bytes)==bytes.size() && file.commit();};
    if (!dir.mkdir("images")) return std::unexpected(QStringLiteral("Could not create training image folder."));
    QJsonArray samples; QByteArray train,validation,paddleTrain,paddleValidation; QSet<char32_t> characters; int count=0;
    for (const auto &s:dataset.samples) if (s.decision=="approved") {
        if (cancelled.load()) return std::unexpected(QStringLiteral("Cancelled"));
        const QString name=QString("images/line-%1").arg(++count,6,10,QChar('0'));
        if (!write(name+".png",s.png) || !write(name+".gt.txt",s.text.toUtf8()+'\n')) return std::unexpected(QStringLiteral("Could not write training image or transcription."));
        const auto path=(name+".png\n").toUtf8(); const auto paddle=(name+".png\t"+s.text+'\n').toUtf8();
        if (s.partition=="validation") {validation+=path; paddleValidation+=paddle;} else {train+=path; paddleTrain+=paddle;}
        for (auto c:s.text.toUcs4()) if (c!=' ') characters.insert(c);
        samples.append(trainingSampleJson(s));
    }
    if (!count) return std::unexpected(QStringLiteral("Approve examples before exporting."));
    auto chars=characters.values(); std::ranges::sort(chars); QByteArray dictionary; for (auto c:chars) dictionary+=QString::fromUcs4(&c,1).toUtf8()+'\n';
    if (!write("dataset.yagf-training.json",QJsonDocument(QJsonObject{{"protocol","yagf-native-training/1"},{"source_id",dataset.sourceId},{"samples",samples}}).toJson()) ||
        !write("train.txt",train) || !write("validation.txt",validation) || !write("paddle-train.txt",paddleTrain) || !write("paddle-validation.txt",paddleValidation) || !write("characters.txt",dictionary))
        return std::unexpected(QStringLiteral("Could not write training manifests."));
    return {};
}
}

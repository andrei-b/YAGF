#include "ocrbenchmark.h"
#include <QRegularExpression>
#include <QSet>
#include <algorithm>
#include <numeric>

namespace yagf::core {
namespace {
QString normalized(QString text) {
    static const QRegularExpression whitespace(QStringLiteral("\\s+"));
    return text.normalized(QString::NormalizationForm_C).replace(whitespace," ").trimmed();
}
template<class Sequence>
std::expected<qint64,QString> distance(const Sequence &left,const Sequence &right,std::stop_token stop)
{
    qsizetype prefix=0,suffix=0;
    while(prefix<left.size() && prefix<right.size() && left[prefix]==right[prefix])++prefix;
    while(suffix<left.size()-prefix && suffix<right.size()-prefix && left[left.size()-suffix-1]==right[right.size()-suffix-1])++suffix;
    const qsizetype n=left.size()-prefix-suffix,m=right.size()-prefix-suffix;
    if(n==0 || m==0)return std::max(n,m);
    if(n>100000 || m>100000 || qint64(n)*m>100000000)
        return std::unexpected(QStringLiteral("Reference passage is too large for bounded scoring; use smaller verified pages."));
    QVector<int> previous(m+1),current(m+1);std::iota(previous.begin(),previous.end(),0);
    for(qsizetype i=1;i<=n;++i) {
        if((i&63)==0 && stop.stop_requested())return std::unexpected(QStringLiteral("Cancelled"));
        current[0]=i;
        for(qsizetype j=1;j<=m;++j)current[j]=std::min({current[j-1]+1,previous[j]+1,previous[j-1]+(left[prefix+i-1]!=right[prefix+j-1])});
        current.swap(previous);
    }
    return previous[m];
}
QJsonObject geometryScores(const OcrPage &reference,const OcrPage &reading)
{
    if(reference.words.size()>5000 || reading.words.size()>5000)return {{"available",false},{"reason","Too many boxes"}};
    for(const auto &word:reference.words)if(word.layoutBlock)return {{"available",false},{"reason","Reference has passage boxes"}};
    for(const auto &word:reading.words)if(word.layoutBlock)return {{"available",false},{"reason","Provider has passage boxes"}};
    QSet<int> used;QVector<int> order;double sum=0;
    for(const auto &word:reference.words) {
        int best=-1;double overlap=.3;
        for(int i=0;i<reading.words.size();++i) {
            if(used.contains(i))continue;
            const QRect other=reading.words[i].bounds,common=word.bounds.intersected(other);
            const double area=double(common.width())*common.height();
            const double unionArea=double(word.bounds.width())*word.bounds.height()+double(other.width())*other.height()-area;
            const double score=unionArea>0?area/unionArea:0;
            if(score>overlap){best=i;overlap=score;}
        }
        if(best>=0){used.insert(best);order.append(best);sum+=overlap;}
    }
    qint64 inversions=0;for(int i=0;i<order.size();++i)for(int j=i+1;j<order.size();++j)if(order[i]>order[j])++inversions;
    const double pairs=double(order.size())*(order.size()-1)/2;
    return {{"available",true},{"reference_boxes",reference.words.size()},{"matched_boxes",used.size()},
            {"iou_coverage",reference.words.isEmpty()?0:sum/reference.words.size()},
            {"reading_order_concordance",pairs>0?QJsonValue(1-inversions/pairs):QJsonValue::Null},
            {"basis","Saved word boxes; geometry was not separately verified"}};
}
}
QJsonObject TextErrorRate::toJson() const {
    return {{"errors",errors},{"reference_units",referenceUnits},{"rate",referenceUnits?QJsonValue(double(errors)/referenceUnits):QJsonValue::Null}};
}
QJsonObject BenchmarkScores::toJson() const {return {{"cer",characters.toJson()},{"wer",words.toJson()},{"geometry",geometry}};}
QString benchmarkReferenceProblem(const OcrPage &page)
{
    if(page.engineId.isEmpty() || page.words.isEmpty())return QStringLiteral("No recognized text");
    if(!page.pendingCorrections.isEmpty())return QStringLiteral("Unresolved saved corrections");
    for(const auto &word:page.words)if(!word.reviewed)return QStringLiteral("Every OCR item must be explicitly verified");
    return {};
}
QString benchmarkText(const OcrPage &page) {OcrDocument doc;doc.pages={page};return normalized(doc.plainText());}
std::expected<BenchmarkScores,QString> scoreRecognition(const OcrPage &reference,const OcrPage &reading,bool compareWordGeometry,std::stop_token stop)
{
    if(stop.stop_requested())return std::unexpected(QStringLiteral("Cancelled"));
    const auto ref=benchmarkText(reference),hyp=benchmarkText(reading);
    const auto refChars=ref.toUcs4(),hypChars=hyp.toUcs4();
    const auto refWords=ref.split(' ',Qt::SkipEmptyParts),hypWords=hyp.split(' ',Qt::SkipEmptyParts);
    const auto chars=distance(refChars,hypChars,stop);if(!chars)return std::unexpected(chars.error());
    const auto words=distance(refWords,hypWords,stop);if(!words)return std::unexpected(words.error());
    BenchmarkScores scores{{*chars,refChars.size()},{*words,refWords.size()},{{"available",false},{"reason","Different coordinate frames or provider granularity"}}};
    if(compareWordGeometry)scores.geometry=geometryScores(reference,reading);
    return scores;
}
}

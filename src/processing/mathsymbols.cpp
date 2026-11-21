#include "mathsymbols.h"
#include "symbolshape.h"

#include <QFontDatabase>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QPainter>
#include <QFontMetrics>
#include <QRawFont>
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <optional>
#include <tuple>

namespace yagf::processing {
namespace {
enum class Kind { Text, Variable, Operator, Fraktur };

Kind codepointKind(char32_t c)
{
    if (c == U'∫' || c == U'∮' || c == U'∑' || c == U'∏') return Kind::Operator;
    if (c == U'𝑥' || c == U'𝑧' || c == U'𝑢' || c == U'𝑣') return Kind::Variable;
    if ((c >= U'𝔄' && c <= U'𝔜' && QChar::isLetter(c)) ||
        (c <= 0xffff && QStringLiteral("ℭℌℑℜℨ").contains(QChar::fromUcs2(c)))) return Kind::Fraktur;
    return Kind::Text;
}

using Mask = std::array<quint64,4>;

Mask maskOf(const QImage &image, QRect box)
{
    Mask result{};
    const auto bytes = symbolShape(image,box);
    if (bytes.size() == sizeof(result)) std::memcpy(result.data(),bytes.constData(),sizeof(result));
    return result;
}

struct Pattern {
    Mask mask;
    Kind kind;
    char32_t letter;
};

QVector<Pattern> patterns()
{
    QVector<Pattern> bank;
    const auto families = QFontDatabase::families();
    for (const auto &name : {"Latin Modern Math","DejaVu Math TeX Gyre","Noto Sans Math",
                              "Liberation Serif","DejaVu Serif","DejaVu Sans"}) {
        if (!families.contains(QLatin1String(name))) continue;
        for (int pixelSize : {20,28,36,44,48,56}) for (bool italic : {false,true}) {
            QFont face{QLatin1String(name)};
            face.setPixelSize(pixelSize);
            face.setItalic(italic);
            const auto font = QRawFont::fromFont(face);
            if (!font.isValid() || (italic && font.style() == QFont::StyleNormal)) continue;
            QString glyphs = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
            if (!italic) {
                glyphs += QStringLiteral("∫∮∑∏𝑥𝑧𝑢𝑣ℭℌℑℜℨ");
                for (char32_t c = U'𝔄'; c <= U'𝔜'; ++c)
                    if (QChar::isLetter(c)) glyphs += QString::fromUcs4(&c,1);
            }
            for (char32_t c : glyphs.toUcs4()) {
                if (!font.supportsCharacter(c)) continue;
                const auto glyph = QString::fromUcs4(&c,1);
                const QRect rect = QFontMetrics(face).tightBoundingRect(glyph).adjusted(-3,-3,3,3);
                QImage image(rect.size(),QImage::Format_Grayscale8);
                image.fill(Qt::white);
                QPainter painter(&image);
                painter.setFont(face);
                painter.drawText(-rect.topLeft(),glyph);
                painter.end();
                cv::Mat tile(image.height(),image.width(),CV_8U,image.bits(),image.bytesPerLine());
                const auto ink = cv::boundingRect(tile < 175);
                if (ink.empty()) continue;
                auto kind = codepointKind(c);
                if (italic && font.style() != QFont::StyleNormal &&
                    (c == 'x' || c == 'z' || c == 'u' || c == 'v')) kind = Kind::Variable;
                const auto mask = maskOf(image,{ink.x,ink.y,ink.width,ink.height});
                const auto normalized = glyph.normalized(QString::NormalizationForm_KC);
                const char32_t letter = kind == Kind::Text ? 0 : kind == Kind::Operator ? c : normalized.front().unicode();
                if (mask != Mask{}) bank.append({mask,kind,letter});
            }
        }
    }
    std::ranges::sort(bank, {}, [](const Pattern &p){return std::tuple(p.kind,p.letter,p.mask);});
    bank.erase(std::unique(bank.begin(),bank.end(),[](const Pattern &a,const Pattern &b) {
        return a.mask == b.mask && a.kind == b.kind && a.letter == b.letter;
    }),bank.end());
    return bank;
}

QString latexFor(Kind kind, char32_t letter)
{
    switch (letter) {
    case U'∑': return QStringLiteral("\\sum");
    case U'∏': return QStringLiteral("\\prod");
    case U'∫': return QStringLiteral("\\int");
    case U'∮': return QStringLiteral("\\oint");
    default: break;
    }
    const auto plain = QString::fromUcs4(&letter,1);
    return kind == Kind::Fraktur ? QStringLiteral("\\mathfrak{%1}").arg(plain) : plain;
}

std::optional<MathSymbol> visualMatch(const QImage &gray, QRect box)
{
    if (box.height() < 8 || box.height() > 240 || box.width() > 2*box.height() ||
        !qobject_cast<QGuiApplication *>(QCoreApplication::instance())) return {};
    QElapsedTimer timer;
    timer.start();
    static const auto bank = patterns();
    static const QString fingerprint = [] {
        QCryptographicHash hash(QCryptographicHash::Sha256);
        for (const auto &p : bank) {
            hash.addData(QByteArray::number(int(p.kind))+":"+QByteArray::number(uint(p.letter))+":");
            hash.addData(QByteArrayView(reinterpret_cast<const char *>(p.mask.data()),sizeof(p.mask)));
        }
        return QString::fromLatin1(hash.result().toHex());
    }();
    const auto mask = maskOf(gray,box);
    double best = 1, ordinary = 1, runnerUp = 1;
    const Pattern *winner = nullptr;
    for (const auto &pattern : bank) {
        int difference = 0, ink = 0;
        for (size_t i = 0; i < mask.size(); ++i) {
            difference += std::popcount(mask[i] ^ pattern.mask[i]);
            ink += std::popcount(mask[i] | pattern.mask[i]);
        }
        const double error = double(difference)/ink;
        if (pattern.kind == Kind::Text) { ordinary = std::min(ordinary,error); continue; }
        const bool same = winner && pattern.kind == winner->kind && pattern.letter == winner->letter;
        if (error < best) {
            if (!same) runnerUp = best;
            best = error;
            winner = &pattern;
        } else if (!same) runnerUp = std::min(runnerUp,error);
    }
    if (!winner || best > 0.32 || best + 0.065 >= ordinary) return {};
    MathSymbol match{box,winner->kind == Kind::Operator};
    match.error = best;
    match.margin = std::min(ordinary,runnerUp) - best;
    const double next = std::min(ordinary,runnerUp);
    const bool distinct = match.margin >= 0.10 || (best <= 0.15 && best <= 0.6*next && match.margin >= 0.065);
    if (best <= 0.25 && distinct) {
        // A detached prime, index or accent can vanish in the normalized
        // mask. Preserve the engine reading whenever the original has islands.
        cv::Mat patch(box.height(),box.width(),CV_8U,
            const_cast<uchar *>(gray.constScanLine(box.y())+box.x()),gray.bytesPerLine());
        cv::Mat labels;
        if (cv::connectedComponents(patch < 175,labels) == 2) {
            match.latex = latexFor(winner->kind,winner->letter);
            match.bankHash = fingerprint;
        }
    }
    match.elapsedMs = timer.elapsed();
    return match;
}
} // namespace

QVector<MathSymbol> findMathSymbols(const QImage &gray, QRect area,
    const QVector<core::OcrWord> &words, bool mathOnly, std::stop_token stop)
{
    QVector<MathSymbol> result;
    area &= gray.rect();
    if (area.isEmpty() || stop.stop_requested()) return result;
    QVector<QRect> occupied;
    for (const auto &word : words) {
        if (!area.intersects(word.bounds)) continue;
        occupied.append(word.bounds);
        const auto token = word.text.trimmed().toUcs4();
        if (word.reviewed || word.layoutBlock || word.geometryInferred || token.size() != 1 ||
            !area.contains(word.bounds)) continue;
        Kind kind = codepointKind(token.first());
        if (kind == Kind::Text && word.italic && QStringLiteral("xzuv").contains(word.text))
            kind = Kind::Variable;
        const QRect box = word.bounds;
        const cv::Mat patch(box.height(),box.width(),CV_8U,
            const_cast<uchar *>(gray.constScanLine(box.y())+box.x()),gray.bytesPerLine());
        const auto ink = cv::boundingRect(patch < 175);
        if (ink.empty()) continue;
        auto match = visualMatch(gray,{box.x()+ink.x,box.y()+ink.y,ink.width,ink.height});
        if (match) {
            match->bounds = box;
            result.append(*match);
        } else if (kind != Kind::Text) result.append({box,kind == Kind::Operator});
        if (stop.stop_requested()) return {};
    }
    // Find missed operators in mixed prose. In a Math block also use isolated
    // letter shapes; the enclosing equation determines the final crop.
    if (!qobject_cast<QGuiApplication *>(QCoreApplication::instance()) ||
        qint64(area.width())*area.height() > 8000000) return result;
    cv::Mat pixels(area.height(),area.width(),CV_8U,
        const_cast<uchar *>(gray.constScanLine(area.y())+area.x()),gray.bytesPerLine());
    cv::Mat mask,labels,stats,centers;
    cv::threshold(pixels,mask,175,255,cv::THRESH_BINARY_INV);
    const int count = cv::connectedComponentsWithStats(mask,labels,stats,centers);
    if (count > 3501) return result;
    for (int i = 1; i < count && !stop.stop_requested(); ++i) {
        const auto *s = stats.ptr<int>(i);
        const QRect box(area.x()+s[cv::CC_STAT_LEFT],area.y()+s[cv::CC_STAT_TOP],
                        s[cv::CC_STAT_WIDTH],s[cv::CC_STAT_HEIGHT]);
        if (box.height() < 12 || std::ranges::any_of(occupied,[&](QRect b){return b.intersects(box);})) continue;
        const auto match = visualMatch(gray,box);
        if (match && (mathOnly || match->largeOperator)) result.append(*match);
    }
    return result;
}
} // namespace yagf::processing

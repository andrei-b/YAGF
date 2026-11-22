#include "pagegeometry.h"
#include <algorithm>
#include <cmath>

namespace yagf::core {
namespace {
// Sample pixel centers with half-open edges. Neighboring blocks can describe
// one edge with different numbers of vertices, so round only after finding
// the intersection with the scanline.
QRegion blockPixels(const QPolygonF &outline, const QRect &canvas) {
    const QRect bounds = enclosingPixels(outline).intersected(canvas);
    QRegion result;
    QVector<double> crossings;
    for (int y=bounds.top(); y<=bounds.bottom(); ++y) {
        const double scan=y+0.5;
        crossings.clear();
        for (int i=0; i<outline.size(); ++i) {
            const auto a=outline[i], b=outline[(i+1)%outline.size()];
            if ((a.y()>scan)==(b.y()>scan)) continue;
            crossings.append(a.x()+(b.x()-a.x())*(scan-a.y())/(b.y()-a.y()));
        }
        std::ranges::sort(crossings);
        for (int i=1; i<crossings.size(); i+=2) {
            const auto edge=[](double x){return int(std::ceil(x-0.5-1e-9));};
            const int left=std::max(bounds.left(),edge(crossings[i-1]));
            const int end=std::min(bounds.right()+1,edge(crossings[i]));
            if (end>left) result |= QRect(left,y,end-left,1);
        }
    }
    return result;
}
std::expected<core::CoordinatePipeline,QString> coordinates(const core::OcrPage &page) {
    if(page.coordinates.isDefined())return page.coordinates;
    return std::unexpected(QStringLiteral("This page has no source-coordinate pipeline."));
}
void attach(QVector<core::RecognitionResult> &readings,const core::CoordinatePipeline &pipeline) {
    for(auto &reading:readings) {
        auto &p=reading.provenance;if(p.sourceToInput.isDefined())continue;
        p.sourceToInput=pipeline;
        const auto area=p.inputArea.isEmpty()?QRect(QPoint(),p.imageSize):p.inputArea;
        p.sourceToInput.appendProjective("recognition-crop",QTransform::fromTranslate(-area.x(),-area.y()),p.imageSize);
    }
}
}
void attachRecognitionCoordinates(core::OcrPage &page) {
    if(!page.coordinates.isDefined())return;
    attach(page.recognitionEvidence,page.coordinates);
    for(auto &reading:page.ocrReadings)attach(reading.recognitionEvidence,page.coordinates);
}
std::expected<core::OcrPage,QString> remapPageGeometry(const core::OcrPage &source,core::CoordinatePipeline destination) {
    auto origin=coordinates(source);if(!origin)return std::unexpected(origin.error());
    if(!origin->validate() || !destination.validate() || origin->outputSize()!=QSizeF(source.width,source.height))
        return std::unexpected(QStringLiteral("Saved coordinates do not match the processed page."));
    if(origin->sourceUnit!=destination.sourceUnit || origin->sourceFrame!=destination.sourceFrame)
        return std::unexpected(QStringLiteral("The original mapping for this restored image is unavailable."));
    if(origin->sourceSize!=destination.sourceSize) {
        if(origin->sourceUnit=="page-points-top-left" && !destination.steps.isEmpty() && destination.steps.first().operation=="render") {
            // Raster dimensions include DPI-dependent integer padding. Preserve
            // physical PDF points rather than stretching them with that padding.
            destination.sourceSize=origin->sourceSize;destination.steps.first().inputSize=origin->sourceSize;
        } else {
        core::CoordinatePipeline aligned;aligned.sourceUnit=origin->sourceUnit;aligned.sourceFrame=origin->sourceFrame;aligned.sourceSize=origin->sourceSize;
        aligned.appendProjective("source-scale",QTransform::fromScale(destination.sourceSize.width()/origin->sourceSize.width(),
            destination.sourceSize.height()/origin->sourceSize.height()),destination.sourceSize);
        aligned.steps+=destination.steps;destination=std::move(aligned);
        }
    }
    core::OcrPage page=source;page.coordinates=*origin;attachRecognitionCoordinates(page);
    if(*origin==destination)return page;
    const QRect canvas(QPoint(),destination.outputSize().toSize());
    QHash<QString,core::GeometryAnchor> anchors;
    const auto map=[&](const QString &key,QRect &box,QVector<QRect> *fragments=nullptr,
                       bool mapShape=false)->std::expected<void,QString> {
        if(box.isEmpty())return {};
        auto anchor=source.sourceGeometry.value(key);
        const auto oldFragments=fragments?*fragments:QVector<QRect>{};
        if(anchor.outline.isEmpty() || anchor.lastBounds!=box || anchor.lastFragments!=oldFragments) {
            auto outline=origin->mapOutline(core::rectangleOutline(QRectF(box)),true);
            if(!outline)return std::unexpected(outline.error());anchor.outline=*outline;anchor.fragments.clear();
            for(const auto &fragment:oldFragments) {
                auto points=origin->mapOutline(core::rectangleOutline(QRectF(fragment)),true);
                if(!points)return std::unexpected(points.error());anchor.fragments.append(*points);
            }
        }
        auto outline=destination.mapOutline(anchor.outline);if(!outline)return std::unexpected(outline.error());
        const auto projected=core::enclosingPixels(*outline).intersected(canvas);
        if(projected.isEmpty())return std::unexpected(QStringLiteral("A transformed region falls outside the page. The existing page was kept."));
        box=projected;
        if(fragments) {
            QRegion projectedShape;
            fragments->clear();for(const auto &fragment:anchor.fragments) {
                auto points=destination.mapOutline(fragment);if(!points)return std::unexpected(points.error());
                if (mapShape) {
                    projectedShape |= blockPixels(*points,box);
                    continue;
                }
                auto bounds=core::enclosingPixels(*points).intersected(box);
                if(bounds.isEmpty())return std::unexpected(QStringLiteral("A source fragment falls outside the transformed page."));
                fragments->append(bounds);
            }
            if (mapShape && !anchor.fragments.isEmpty()) {
                if (projectedShape.isEmpty() || projectedShape.rectCount()>4096)
                    return std::unexpected(QStringLiteral("The transformed block outline cannot be preserved."));
                for (QRect piece : projectedShape) fragments->append(piece);
            }
        }
        anchor.lastBounds=box;anchor.lastFragments=fragments?*fragments:QVector<QRect>{};anchors.insert(key,std::move(anchor));return {};
    };
    const auto words=[&](auto &items,const QString &prefix)->std::expected<void,QString> {
        for(auto &word:items) {
            auto mapped=map(prefix+word.id,word.bounds,&word.sourceBoxes);if(!mapped)return mapped;
            word.symbolShape.clear(); // A mask sampled in an old frame cannot be reused as a new crop.
        }return {};
    };
    if(auto r=words(page.words,"word/");!r)return std::unexpected(r.error());
    if(auto r=words(page.pendingCorrections,"pending/");!r)return std::unexpected(r.error());
    if(auto r=words(page.deletedWords,"deleted/");!r)return std::unexpected(r.error());
    for(auto &region:page.regions) {
        if(auto r=map("region/"+region.id,region.bounds);!r)return std::unexpected(r.error());
        for(auto &paragraph:region.paragraphs) {
            if(auto r=map("paragraph/"+paragraph.id,paragraph.bounds);!r)return std::unexpected(r.error());
            for(auto &line:paragraph.lines)if(auto r=map("line/"+line.id,line.bounds);!r)return std::unexpected(r.error());
        }
    }
    for (auto &region : page.imageRegions) {
        for (auto &part : region.parts) part.translate(region.bounds.topLeft());
        if (auto r=map("image/"+region.id,region.bounds,&region.parts,true); !r) return std::unexpected(r.error());
        for (auto &part : region.parts) part.translate(-region.bounds.topLeft());
    }
    for(auto &reading:page.ocrReadings) {
        if(auto r=map("reading/"+reading.source,reading.area);!r)return std::unexpected(r.error());
        for(int i=0;i<reading.items.size();++i)
            if(auto r=map("reading/"+reading.source+"/"+QString::number(i),reading.items[i].bounds);!r)return std::unexpected(r.error());
    }
    page.width=canvas.width();page.height=canvas.height();page.coordinates=std::move(destination);page.sourceGeometry=std::move(anchors);
    return page;
}

std::expected<QPolygonF,QString> recognitionOutlineOnPage(const core::RecognitionResult &reading,
    const core::RecognitionRegion &region,const core::CoordinatePipeline &current) {
    const auto &frame=reading.provenance.sourceToInput;
    if(!frame.isDefined() || !frame.validate() || !current.validate() || frame.sourceUnit!=current.sourceUnit ||
        frame.sourceFrame!=current.sourceFrame || (frame.sourceSize!=current.sourceSize && frame.sourceUnit!="page-points-top-left"))
        return std::unexpected(QStringLiteral("Recognition input has no compatible source-coordinate mapping."));
    if(!region.bounds)return std::unexpected(QStringLiteral("This model did not provide region coordinates."));
    auto original=frame.mapOutline(core::rectangleOutline(QRectF(*region.bounds)),true);
    if(!original)return std::unexpected(original.error());return current.mapOutline(*original);
}
}

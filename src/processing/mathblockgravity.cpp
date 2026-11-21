#include "mathblockgravity.h"
#include <QSet>
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <new>

namespace yagf::processing {
namespace {
bool textBlock(const core::ImageRegion &r) {
    return !r.notText && (r.content.isEmpty() || r.content=="text" || r.content=="mixed");
}
bool mathBlock(const core::ImageRegion &r) { return !r.notText && r.content=="math"; }
struct Component {
    QRect box;
    int owner=-1, pixels=0;
    bool border=false;
};
int typicalHeight(QVector<int> values) {
    if(values.isEmpty())return 0;
    auto middle=values.begin()+values.size()/2;std::nth_element(values.begin(),middle,values.end());return *middle;
}
cv::Mat distances(const cv::Mat &labels,const QVector<bool> &anchors) {
    cv::Mat free(labels.size(),CV_8U,cv::Scalar(255));
    bool any=false;
    for(int y=0;y<labels.rows;++y) {
        const auto *ids=labels.ptr<int>(y);auto *row=free.ptr<uchar>(y);
        for(int x=0;x<labels.cols;++x)if(anchors[ids[x]]){row[x]=0;any=true;}
    }
    if(!any)return {};
    cv::Mat result;cv::distanceTransform(free,result,cv::DIST_L2,cv::DIST_MASK_PRECISE);return result;
}
double componentDistance(int id,const Component &component,const cv::Mat &labels,const cv::Mat &distance,QPoint offset) {
    if(distance.empty())return std::numeric_limits<double>::infinity();
    const QRect box=component.box.translated(-offset);double total=0;int count=0;
    for(int y=box.top();y<=box.bottom();++y) {
        const auto *ids=labels.ptr<int>(y);const auto *row=distance.ptr<float>(y);
        for(int x=box.left();x<=box.right();++x)if(ids[x]==id){total+=row[x];++count;}
    }
    return count?total/count:std::numeric_limits<double>::infinity();
}
// The rectangular editor can represent only an edge cut. Never remove a whole
// line, cut another glyph, or drop a donor block just to absorb a small mark.
QRect remainingInk(const QImage &gray,const QRect &box,int threshold,const QRect &roi,
                   const cv::Mat &labels,const QSet<int> &moved,const QVector<Component> &components,int padding,std::stop_token stop) {
    QRect ink;
    QVector<QRect> halos;for(int id:moved)halos.append(components[id].box.adjusted(-padding,-padding,padding,padding));
    for(int y=box.top();y<=box.bottom();++y) {
        if(stop.stop_requested())return {};
        const auto *row=gray.constScanLine(y);
        for(int x=box.left();x<=box.right();++x)if(row[x]<=threshold) {
            if(roi.contains(x,y)) {
                const int label=labels.at<int>(y-roi.y(),x-roi.x());
                if(moved.contains(label))continue;
                // Include faint antialiased edges around a transferred glyph,
                // without treating a nearby component's ink as part of it.
                if(!label && std::ranges::any_of(halos,[&](const auto &halo){return halo.contains(x,y);}))continue;
            }
            ink=ink.united(QRect(x,y,1,1));
        }
    }
    return ink;
}
QRect trimmedBox(const QRect &original,const QRect &math,const QRect &ink) {
    if(ink.isEmpty())return {};
    const QVector<QRect> options{
        QRect(original.topLeft(),QPoint(original.right(),std::min(original.bottom(),math.top()-1))),
        QRect(QPoint(original.left(),std::max(original.top(),math.bottom()+1)),original.bottomRight()),
        QRect(original.topLeft(),QPoint(std::min(original.right(),math.left()-1),original.bottom())),
        QRect(QPoint(std::max(original.left(),math.right()+1),original.top()),original.bottomRight())};
    QRect best;
    for(const auto &option:options)if(!option.isEmpty() && original.contains(option) && option.contains(ink) &&
        qint64(option.width())*option.height()>qint64(best.width())*best.height())best=option;
    return best;
}
}

std::expected<MathBlockRefinement,QString> refineMathBlocks(
    const QImage &image,const QVector<core::ImageRegion> &input,std::stop_token stop) try {
    const auto cancelled=[] {return std::unexpected(QStringLiteral("Block refinement cancelled."));};
    if(stop.stop_requested())return cancelled();
    if(image.isNull() || image.width()>20000 || image.height()>20000 || qint64(image.width())*image.height()>80000000 || input.size()>1000)
        return std::unexpected(QStringLiteral("The page is empty or too large for block refinement."));
    for(const auto &r:input)if(r.bounds.isEmpty() || !image.rect().contains(r.bounds))
        return std::unexpected(QStringLiteral("A block lies outside the displayed page."));
    MathBlockRefinement result{input};
    if(std::ranges::none_of(input,mathBlock))return result;
    QImage gray=image.convertToFormat(QImage::Format_Grayscale8);
    if(gray.isNull())return std::unexpected(QStringLiteral("Could not prepare the page for block refinement."));
    // Transparent paper must not turn into black connected components.
    if(image.hasAlphaChannel())for(int y=0;y<image.height();++y) {
        if(stop.stop_requested())return cancelled();auto *row=gray.scanLine(y);
        for(int x=0;x<image.width();++x)if(qAlpha(image.pixel(x,y))<255) {
            const auto color=image.pixelColor(x,y);const int alpha=color.alpha();
            row[x]=(qGray(color.rgb())*alpha+255*(255-alpha))/255;
        }
    }
    for(int target=0;target<result.regions.size();++target) {
        if(stop.stop_requested())return cancelled();
        if(!mathBlock(result.regions[target]))continue;
        const QRect original=result.regions[target].bounds;
        const int halo=2*std::clamp(original.height()/2,12,160);
        const QRect roi=original.adjusted(-halo,-halo,halo,halo).intersected(image.rect());
        if(qint64(roi.width())*roi.height()>4000000)continue;
        cv::Mat pixels(roi.height(),roi.width(),CV_8U,
            const_cast<uchar *>(gray.constScanLine(roi.y())+roi.x()),gray.bytesPerLine());
        cv::Mat binary,labels,stats,centroids;
        int threshold=int(cv::threshold(pixels,binary,0,255,cv::THRESH_BINARY_INV|cv::THRESH_OTSU));
        if(threshold>220){threshold=220;cv::threshold(pixels,binary,threshold,255,cv::THRESH_BINARY_INV);}
        // The ownership test uses strong ink; the final boundary guard also
        // protects faint prose that Otsu may have grouped with the paper.
        std::array<int,256> histogram{};
        for(int y=0;y<pixels.rows;++y) {
            if(stop.stop_requested())return cancelled();const auto *row=pixels.ptr<uchar>(y);
            for(int x=0;x<pixels.cols;++x)++histogram[row[x]];
        }
        int paper=255,total=0;for(int value=0;value<256;++value) {
            total+=histogram[value];if(total>=pixels.rows*pixels.cols*.9){paper=value;break;}
        }
        const int protectedThreshold=std::max(threshold,paper-8);
        const int count=cv::connectedComponentsWithStats(binary,labels,stats,centroids,8,CV_32S);
        if(count>20000)continue;
        QVector<Component> components(count);QVector<QVector<int>> heights(input.size());
        for(int id=1;id<count;++id) {
            auto &c=components[id];c.box={stats.at<int>(id,cv::CC_STAT_LEFT)+roi.x(),stats.at<int>(id,cv::CC_STAT_TOP)+roi.y(),
                stats.at<int>(id,cv::CC_STAT_WIDTH),stats.at<int>(id,cv::CC_STAT_HEIGHT)};
            c.pixels=stats.at<int>(id,cv::CC_STAT_AREA);
            c.border=c.box.left()==roi.left() || c.box.right()==roi.right() || c.box.top()==roi.top() || c.box.bottom()==roi.bottom();
            for(int owner=0;owner<result.regions.size();++owner)if(result.regions[owner].bounds.contains(c.box)) {
                if(c.owner!=-1){c.owner=-2;break;}c.owner=owner;
            }
            if(c.owner>=0 && !c.border && c.box.height()>=3 && c.pixels>=3)heights[c.owner].append(c.box.height());
        }
        QVector<int> fontHeights;for(const auto &values:heights)fontHeights.append(typicalHeight(values));
        const int mathHeight=fontHeights[target];if(mathHeight<4)continue;
        const int reach=std::min(halo/2,int(std::ceil(mathHeight*1.25)));
        const int padding=std::max(1,mathHeight/20);
        const QRect neighborhood=original.adjusted(-reach,-reach,reach,reach);
        QVector<bool> mathAnchors(count),otherMath(count);QVector<int> candidates;
        const auto small=[&](const Component &c,int h) {
            return !c.border && c.pixels>=2 && c.box.height()<=std::max(3,int(h*.8)) &&
                c.box.width()<=std::max(4,int(h*1.5)) && c.pixels<=h*h*.7;
        };
        for(int id=1;id<count;++id) {
            const auto &c=components[id];
            if(c.owner==target)mathAnchors[id]=true;
            else if(c.owner>=0 && mathBlock(result.regions[c.owner]))otherMath[id]=true;
            else if(c.owner>=0 && textBlock(result.regions[c.owner]) && neighborhood.contains(c.box) &&
                small(c,std::min(mathHeight,fontHeights[c.owner])))candidates.append(id);
        }
        if(candidates.isEmpty())continue;
        std::stable_sort(candidates.begin(),candidates.end(),[&](int a,int b){
            const auto &left=components[a].box;const auto &right=components[b].box;
            return left.top()!=right.top()?left.top()<right.top():left.left()<right.left();
        });
        const auto mathDistance=distances(labels,mathAnchors);if(mathDistance.empty())continue;
        const auto competingDistance=distances(labels,otherMath);
        QSet<int> eligible;
        QSet<int> owners;for(int id:candidates)owners.insert(components[id].owner);
        if(owners.size()>32)continue;
        for(int owner:owners) {
            if(stop.stop_requested())return cancelled();
            const int h=std::min(mathHeight,fontHeights[owner]);
            QVector<bool> body(count);
            for(int id=1;id<count;++id)if(components[id].owner==owner && !small(components[id],h))body[id]=true;
            const auto textDistance=distances(labels,body);if(textDistance.empty())continue;
            for(int id:candidates)if(components[id].owner==owner) {
                const double formula=componentDistance(id,components[id],labels,mathDistance,roi.topLeft());
                const double text=componentDistance(id,components[id],labels,textDistance,roi.topLeft());
                const double other=componentDistance(id,components[id],labels,competingDistance,roi.topLeft());
                const double margin=std::max(1.,h*.12);
                if(formula<=reach && formula+margin<text && formula<text*.75 && other>formula*1.2+margin)eligible.insert(id);
            }
        }
        QSet<int> accepted;
        // Read-order iteration makes ties deterministic. Never promote a mark
        // merely because another mark was moved earlier in this pass.
        for(int id:candidates)if(eligible.contains(id) && !accepted.contains(id)) {
            QSet<int> proposed=accepted;proposed.insert(id);QRect expanded=original;
            for(int part:proposed)expanded=expanded.united(components[part].box.adjusted(-padding,-padding,padding,padding).intersected(image.rect()));
            bool safe=true,again=true;
            while(again && safe) {
                again=false;
                for(int part=1;part<count;++part)if(!original.contains(components[part].box) && components[part].box.intersects(expanded) && !proposed.contains(part)) {
                    if(!eligible.contains(part)){safe=false;break;}
                    proposed.insert(part);expanded=expanded.united(components[part].box.adjusted(-padding,-padding,padding,padding).intersected(image.rect()));again=true;
                }
            }
            if(!safe)continue;
            QVector<QRect> boxes;for(const auto &r:result.regions)boxes.append(r.bounds);
            for(int owner=0;owner<boxes.size() && safe;++owner)if(owner!=target && boxes[owner].intersects(expanded)) {
                if(!textBlock(result.regions[owner])){safe=false;break;}
                const auto ink=remainingInk(gray,boxes[owner],protectedThreshold,roi,labels,proposed,components,padding,stop);
                const auto protectedInk=ink.isEmpty()?ink:ink.adjusted(-1,-1,1,1).intersected(boxes[owner]);
                const auto trimmed=trimmedBox(boxes[owner],expanded,protectedInk);
                if(trimmed.isEmpty())safe=false;else boxes[owner]=trimmed;
            }
            if(stop.stop_requested())return cancelled();
            if(!safe)continue;
            boxes[target]=expanded;
            for(int owner=0;owner<boxes.size();++owner)result.regions[owner].bounds=boxes[owner];
            accepted=std::move(proposed);
        }
        result.movedComponents+=accepted.size();
    }
    for(int i=0;i<input.size();++i)if(result.regions[i].bounds!=input[i].bounds) {
        ++result.changedBlocks;auto &r=result.regions[i];r.autoDetected=false;r.detectedLabel.clear();r.detectionScore=-1;
    }
    if(stop.stop_requested())return cancelled();
    return result;
} catch(const cv::Exception &) {
    return std::unexpected(QStringLiteral("Could not analyze page ink for block refinement."));
} catch(const std::bad_alloc &) {
    return std::unexpected(QStringLiteral("Not enough memory to refine these blocks."));
}
}

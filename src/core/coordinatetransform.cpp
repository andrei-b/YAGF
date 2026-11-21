#include "coordinatetransform.h"
#include <QJsonArray>
#include <algorithm>
#include <cmath>

namespace yagf::core {
namespace {
bool finite(QPointF p) { return std::isfinite(p.x()) && std::isfinite(p.y()) && std::abs(p.x())<1e9 && std::abs(p.y())<1e9; }
bool sizeValid(QSizeF s) { return std::isfinite(s.width()) && std::isfinite(s.height()) && s.width()>0 && s.height()>0 && s.width()<=1000000 && s.height()<=1000000; }
double cross(QPointF a, QPointF b) { return a.x()*b.y()-a.y()*b.x(); }
QPointF displaced(const CoordinateStep &step, QPointF p) {
    const double x=p.x()/step.inputSize.width()*(step.gridSize.width()-1), y=p.y()/step.inputSize.height()*(step.gridSize.height()-1);
    const int col=int(std::clamp(std::floor(x),0.,double(step.gridSize.width()-2)));
    const int row=int(std::clamp(std::floor(y),0.,double(step.gridSize.height()-2)));
    const double u=x-col,v=y-row;
    const auto at=[&](int c,int r){return step.displacement[r*step.gridSize.width()+c];};
    return p+at(col,row)*(1-u)*(1-v)+at(col+1,row)*u*(1-v)+at(col,row+1)*(1-u)*v+at(col+1,row+1)*u*v;
}
std::expected<QPointF,QString> inverseMesh(const CoordinateStep &step,QPointF target) {
    QPointF p(target.x()*step.inputSize.width()/step.outputSize.width(),target.y()*step.inputSize.height()/step.outputSize.height());
    for (int iteration=0;iteration<48;++iteration) {
        const auto error=displaced(step,p)-target;
        if (std::hypot(error.x(),error.y())<1e-7) return p;
        constexpr double delta=.01;
        const auto dx=(displaced(step,p+QPointF(delta,0))-displaced(step,p-QPointF(delta,0)))/(2*delta);
        const auto dy=(displaced(step,p+QPointF(0,delta))-displaced(step,p-QPointF(0,delta)))/(2*delta);
        const double determinant=cross(dx,dy);
        if (std::abs(determinant)<1e-10) break;
        const QPointF correction(cross(error,dy)/determinant,cross(dx,error)/determinant);
        p-=correction;
        if (!finite(p)) break;
    }
    return std::unexpected(QStringLiteral("Could not invert the dewarping field."));
}
QJsonArray pointsJson(const QPolygonF &points) {QJsonArray out;for(auto p:points)out.append(QJsonArray{p.x(),p.y()});return out;}
bool readPoints(const QJsonValue &value,QPolygonF &points,int limit=4096) {
    if(!value.isArray() || value.toArray().size()>limit)return false;
    for(const auto &item:value.toArray()) {
        if(!item.isArray() || item.toArray().size()!=2 || !item[0].isDouble() || !item[1].isDouble())return false;
        QPointF p(item[0].toDouble(),item[1].toDouble());if(!finite(p))return false;points.append(p);
    }return true;
}
QJsonArray rectJson(QRect r) {return {r.x(),r.y(),r.width(),r.height()};}
bool readRect(const QJsonValue &v,QRect &r) {
    if(!v.isArray() || v.toArray().size()!=4)return false;
    for(const auto &n:v.toArray())if(!n.isDouble() || !std::isfinite(n.toDouble()) || n.toDouble()!=std::floor(n.toDouble()) || std::abs(n.toDouble())>1000000)return false;
    r={v[0].toInt(),v[1].toInt(),v[2].toInt(),v[3].toInt()};return !r.isEmpty();
}
}

void CoordinatePipeline::appendProjective(QString operation,QTransform mapping,QSizeF output) {
    steps.append({.operation=std::move(operation),.inputSize=outputSize(),.outputSize=output,.projective=mapping});
}
std::expected<void,QString> CoordinatePipeline::validate() const {
    const auto fail=[] {return std::unexpected(QStringLiteral("Invalid coordinate transformation pipeline."));};
    if(!sizeValid(sourceSize) || (sourceUnit!="pixels" && sourceUnit!="page-points-top-left") ||
        (sourceFrame!="original" && sourceFrame!="restored" && sourceFrame!="ocr-input") || steps.size()>32)return fail();
    QSizeF prior=sourceSize;
    for(const auto &s:steps) {
        if(s.operation.isEmpty() || s.operation.size()>64 || s.inputSize!=prior || !sizeValid(s.outputSize))return fail();
        prior=s.outputSize;
        if(s.displacement.isEmpty()) {
            const double m[]={s.projective.m11(),s.projective.m12(),s.projective.m13(),s.projective.m21(),s.projective.m22(),s.projective.m23(),s.projective.m31(),s.projective.m32(),s.projective.m33()};
            for(double v:m)if(!std::isfinite(v) || std::abs(v)>1e9)return fail();
            if(!s.projective.isInvertible())return fail();
            double sign=0;
            for(auto p:rectangleOutline(QRectF(QPointF(),s.inputSize))) {
                const double w=s.projective.m13()*p.x()+s.projective.m23()*p.y()+s.projective.m33();
                if(std::abs(w)<1e-9 || (sign && sign*w<0) || !finite(s.projective.map(p)))return fail();sign=w;
            }
        } else {
            if(!s.projective.isIdentity() || s.gridSize.width()<2 || s.gridSize.height()<2 || s.gridSize.width()>257 || s.gridSize.height()>257 ||
                s.displacement.size()!=s.gridSize.width()*s.gridSize.height())return fail();
            for(auto p:s.displacement)if(!finite(p))return fail();
            const auto node=[&](int c,int r) {return QPointF(c*s.inputSize.width()/(s.gridSize.width()-1),r*s.inputSize.height()/(s.gridSize.height()-1))+s.displacement[r*s.gridSize.width()+c];};
            // Reject folded or degenerate bilinear cells before accepting a field.
            for(int r=0;r<s.gridSize.height()-1;++r)for(int c=0;c<s.gridSize.width()-1;++c) {
                const auto a=node(c,r),b=node(c+1,r),d=node(c,r+1),e=node(c+1,r+1);
                if(cross(b-a,d-a)<=1e-8 || cross(b-a,e-b)<=1e-8 || cross(e-d,d-a)<=1e-8 || cross(e-d,e-b)<=1e-8)return fail();
            }
            QPolygonF boundary;
            for(int c=0;c<s.gridSize.width();++c)boundary.append(node(c,0));
            for(int r=1;r<s.gridSize.height();++r)boundary.append(node(s.gridSize.width()-1,r));
            for(int c=s.gridSize.width()-2;c>=0;--c)boundary.append(node(c,s.gridSize.height()-1));
            for(int r=s.gridSize.height()-2;r>0;--r)boundary.append(node(0,r));
            for(int i=0;i<boundary.size();++i)for(int j=i+2;j<boundary.size();++j) {
                if(i==0 && j==boundary.size()-1)continue;
                if(QLineF(boundary[i],boundary[(i+1)%boundary.size()]).intersects(
                    QLineF(boundary[j],boundary[(j+1)%boundary.size()]),nullptr)==QLineF::BoundedIntersection)return fail();
            }
        }
    }return {};
}
std::expected<QPointF,QString> CoordinatePipeline::fromSource(QPointF p) const {
    if(!finite(p))return std::unexpected(QStringLiteral("Invalid source coordinate."));
    for(const auto &s:steps){
        if(!s.displacement.isEmpty() && (s.gridSize.width()<2 || s.gridSize.height()<2 || s.displacement.size()!=s.gridSize.width()*s.gridSize.height()))
            return std::unexpected(QStringLiteral("Invalid dewarping field."));
        p=s.displacement.isEmpty()?s.projective.map(p):displaced(s,p);if(!finite(p))return std::unexpected(QStringLiteral("Coordinate mapping is unbounded."));
    }return p;
}
std::expected<QPointF,QString> CoordinatePipeline::toSource(QPointF p) const {
    if(!finite(p))return std::unexpected(QStringLiteral("Invalid processed coordinate."));
    for(auto it=steps.crbegin();it!=steps.crend();++it) {
        if(it->displacement.isEmpty()) {bool ok=false;const auto inverse=it->projective.inverted(&ok);if(!ok)return std::unexpected(QStringLiteral("Coordinate mapping is singular."));p=inverse.map(p);}
        else {
            if(it->gridSize.width()<2 || it->gridSize.height()<2 || it->displacement.size()!=it->gridSize.width()*it->gridSize.height())return std::unexpected(QStringLiteral("Invalid dewarping field."));
            auto inverse=inverseMesh(*it,p);if(!inverse)return inverse;p=*inverse;
        }
        if(!finite(p))return std::unexpected(QStringLiteral("Coordinate mapping is unbounded."));
    }return p;
}
std::expected<QPolygonF,QString> CoordinatePipeline::mapOutline(const QPolygonF &points,bool inverse) const {
    QPolygonF result;
    const bool nonlinear=std::ranges::any_of(steps,[](const auto &s){return !s.displacement.isEmpty();});
    for(int i=0;i<points.size();++i) {
        const auto a=points[i],b=points[(i+1)%points.size()];
        const int samples=nonlinear && points.size()<=8?32:1;
        for(int j=0;j<samples;++j) {
            auto mapped=inverse?toSource(a+(b-a)*(double(j)/samples)):fromSource(a+(b-a)*(double(j)/samples));
            if(!mapped)return std::unexpected(mapped.error());result.append(*mapped);
        }
    }return result;
}
QPolygonF rectangleOutline(QRectF r) {return {r.topLeft(),r.topRight(),r.bottomRight(),r.bottomLeft()};}
QRect enclosingPixels(const QPolygonF &outline) {
    if(outline.isEmpty())return {};
    const auto b=outline.boundingRect();constexpr double epsilon=1e-6;
    const int x=int(std::floor(b.left()+epsilon)),y=int(std::floor(b.top()+epsilon));
    return {x,y,int(std::ceil(b.right()-epsilon))-x,int(std::ceil(b.bottom()-epsilon))-y};
}
QJsonObject CoordinatePipeline::toJson() const {
    QJsonArray out;
    for(const auto &s:steps) {
        QJsonObject v{{"operation",s.operation},{"input",QJsonArray{s.inputSize.width(),s.inputSize.height()}},{"output",QJsonArray{s.outputSize.width(),s.outputSize.height()}}};
        if(s.displacement.isEmpty())v["matrix"]=QJsonArray{s.projective.m11(),s.projective.m12(),s.projective.m13(),s.projective.m21(),s.projective.m22(),s.projective.m23(),s.projective.m31(),s.projective.m32(),s.projective.m33()};
        else {v["grid"]=QJsonArray{s.gridSize.width(),s.gridSize.height()};v["displacement"]=pointsJson(QPolygonF(s.displacement));}
        out.append(v);
    }return {{"source_unit",sourceUnit},{"source_frame",sourceFrame},{"source_size",QJsonArray{sourceSize.width(),sourceSize.height()}},{"steps",out}};
}
std::expected<CoordinatePipeline,QString> CoordinatePipeline::fromJson(const QJsonObject &json) {
    const auto fail=[] {return std::unexpected(QStringLiteral("Invalid saved coordinate pipeline."));};
    const auto size=[](const QJsonValue &v)->QSizeF {if(!v.isArray() || v.toArray().size()!=2 || !v[0].isDouble() || !v[1].isDouble())return {};return {v[0].toDouble(),v[1].toDouble()};};
    CoordinatePipeline out;out.sourceUnit=json["source_unit"].toString();out.sourceSize=size(json["source_size"]);
    out.sourceFrame=json.value("source_frame").toString("original");
    if(!json["steps"].isArray() || json["steps"].toArray().size()>32)return fail();
    for(const auto &value:json["steps"].toArray()) {
        if(!value.isObject())return fail();const auto v=value.toObject();CoordinateStep step;
        step.operation=v["operation"].toString();step.inputSize=size(v["input"]);step.outputSize=size(v["output"]);
        if(v.contains("matrix")) {
            if(v.contains("displacement") || !v["matrix"].isArray() || v["matrix"].toArray().size()!=9)return fail();
            const auto m=v["matrix"].toArray();for(const auto &n:m)if(!n.isDouble())return fail();
            step.projective=QTransform(m[0].toDouble(),m[1].toDouble(),m[2].toDouble(),m[3].toDouble(),m[4].toDouble(),m[5].toDouble(),m[6].toDouble(),m[7].toDouble(),m[8].toDouble());
        } else {
            const auto grid=size(v["grid"]);if(grid.width()!=std::floor(grid.width()) || grid.height()!=std::floor(grid.height()) || grid.width()>257 || grid.height()>257)return fail();
            step.gridSize=grid.toSize();QPolygonF points;if(!readPoints(v["displacement"],points,257*257))return fail();step.displacement=points;
        }out.steps.append(std::move(step));
    }
    if(auto valid=out.validate();!valid)return std::unexpected(valid.error());return out;
}
QJsonObject geometryAnchorsJson(const QHash<QString,GeometryAnchor> &anchors) {
    QJsonObject out;
    for(auto it=anchors.cbegin();it!=anchors.cend();++it) {
        QJsonArray fragments,last;for(const auto &f:it->fragments)fragments.append(pointsJson(f));for(const auto &f:it->lastFragments)last.append(rectJson(f));
        out[it.key()]=QJsonObject{{"outline",pointsJson(it->outline)},{"fragments",fragments},{"last_box",rectJson(it->lastBounds)},{"last_fragments",last}};
    }return out;
}
std::expected<QHash<QString,GeometryAnchor>,QString> readGeometryAnchors(const QJsonObject &json) {
    const auto fail=[] {return std::unexpected(QStringLiteral("Invalid source geometry anchors."));};
    if(json.size()>100000)return fail();QHash<QString,GeometryAnchor> result;qint64 points=0;
    for(auto it=json.begin();it!=json.end();++it) {
        if(it.key().isEmpty() || it.key().size()>2048 || !it->isObject())return fail();const auto v=it->toObject();GeometryAnchor anchor;
        if(!readPoints(v["outline"],anchor.outline) || anchor.outline.size()<4 || !readRect(v["last_box"],anchor.lastBounds) ||
            !v["fragments"].isArray() || !v["last_fragments"].isArray() || v["fragments"].toArray().size()>4096 || v["last_fragments"].toArray().size()>4096)return fail();
        points+=anchor.outline.size();
        for(const auto &f:v["fragments"].toArray()){QPolygonF p;if(!readPoints(f,p) || p.size()<4)return fail();points+=p.size();anchor.fragments.append(p);}
        for(const auto &f:v["last_fragments"].toArray()){QRect r;if(!readRect(f,r))return fail();anchor.lastFragments.append(r);}
        if(points>2000000 || anchor.fragments.size()!=anchor.lastFragments.size())return fail();result.insert(it.key(),std::move(anchor));
    }return result;
}
} // namespace yagf::core

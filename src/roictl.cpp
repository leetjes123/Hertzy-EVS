#include "roictl.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace evs {

static std::vector<QPointF> to_pts(const QVariantList& l) {
    std::vector<QPointF> p;
    for (const QVariant& v : l) {
        if (v.canConvert<QPointF>() && v.typeId() == QMetaType::QPointF) { p.push_back(v.toPointF()); continue; }
        const QVariantMap m = v.toMap();
        if (m.contains("x")) p.emplace_back(m["x"].toDouble(), m["y"].toDouble());
    }
    return p;
}

QVariantList RoiController::roiList() const {
    QVariantList l;
    for (const Roi& r : rs_.rois) {
        QVariantMap m = roi_to_variant(r);
        m["selected"] = r.id == rs_.selected;
        l << m;
    }
    return l;
}

int RoiController::addRoi(const QString& type, const QVariantList& pts) {
    Roi r;
    r.type = type == "rect" ? RoiType::Rect : type == "polygon" ? RoiType::Polygon : type == "mask" ? RoiType::Mask : RoiType::Line;
    r.pts = to_pts(pts);
    r.source = "drawn";
    if (r.type == RoiType::Line) {
        if (r.pts.size() < 2 || r.length() < 4) return -1;
        r.pts.resize(2);
        r.half = 6;
    } else if (r.type == RoiType::Rect) {
        if (r.pts.size() < 2) return -1;
        r.pts.resize(2);
        const QRectF b = QRectF(r.pts[0], r.pts[1]).normalized();
        if (b.width() < 3 || b.height() < 3) return -1;
        r.pts = {b.topLeft(), b.bottomRight()};
    } else if (r.type == RoiType::Polygon) {
        if (r.pts.size() < 3) return -1;
    } else {
        r.maskW = sensorW();
        r.maskH = sensorH();
        r.mask.assign((size_t)r.maskW * r.maskH, 0);
        r.pts.clear();
    }
    const int id = rs_.add(std::move(r));
    rs_.selected = id;
    edited(true);
    return id;
}

void RoiController::moveRoi(int id, const QVariantList& pts) {
    Roi* r = rs_.find(id);
    if (!r || r->type == RoiType::Mask) return;
    std::vector<QPointF> p = to_pts(pts);
    if (p.size() != r->pts.size()) return;
    if (r->type == RoiType::Rect) { const QRectF b = QRectF(p[0], p[1]).normalized(); p = {b.topLeft(), b.bottomRight()}; }
    r->pts = p;
    edited(true);
}

void RoiController::translateRoi(int id, double dx, double dy) {
    Roi* r = rs_.find(id);
    if (!r || (dx == 0 && dy == 0)) return;
    for (QPointF& p : r->pts) p += QPointF(dx, dy);
    if (r->type == RoiType::Mask) {
        const int ix = (int)std::lround(dx), iy = (int)std::lround(dy);
        if (ix == 0 && iy == 0) return;
        std::vector<uint8_t> m(r->mask.size(), 0);
        for (int y = 0; y < r->maskH; ++y)
            for (int x = 0; x < r->maskW; ++x) {
                if (!r->mask[(size_t)y * r->maskW + x]) continue;
                const int nx = x + ix, ny = y + iy;
                if (nx >= 0 && ny >= 0 && nx < r->maskW && ny < r->maskH) m[(size_t)ny * r->maskW + nx] = 1;
            }
        r->mask.swap(m);
    }
    edited(true);
}

void RoiController::setRoiProp(int id, const QString& key, const QVariant& v) {
    Roi* r = rs_.find(id);
    if (!r) return;
    bool geo = false;
    if (key == "name") { const QString n = v.toString().trimmed(); if (!n.isEmpty()) r->name = n.left(24); }
    else if (key == "color") r->color = QColor(v.toString());
    else if (key == "half") { r->half = std::clamp(v.toDouble(), 1.0, 200.0); geo = true; }
    else if (key == "signal") { r->signal = signal_from(v.toString()); geo = true; }
    else return;
    edited(geo);
}

void RoiController::removeRoi(int id) {
    if (rs_.remove(id)) edited(true);
}

void RoiController::clearRois() {
    if (rs_.rois.empty()) return;
    rs_.clear();
    edited(true);
}

void RoiController::selectRoi(int id) {
    if (rs_.selected == id) return;
    rs_.selected = rs_.find(id) ? id : -1;
    edited(false);
}

int RoiController::duplicateRoi(int id) {
    const Roi* r = rs_.find(id);
    if (!r) return -1;
    Roi c = *r;
    c.id = 0;
    c.name.clear();
    c.color = QColor();
    for (QPointF& p : c.pts) p += QPointF(12, 12);
    const int nid = rs_.add(std::move(c));
    rs_.selected = nid;
    if (Roi* n = rs_.find(nid); n && n->type == RoiType::Mask) { stroking_ = false; translateRoi(nid, 12, 12); return nid; }
    edited(true);
    return nid;
}

int RoiController::paintMask(int id, double x, double y, double rad, bool erase) {
    Roi* r = rs_.find(id);
    if (!r || r->type != RoiType::Mask) {
        if (erase) return -1;
        id = addRoi("mask", {});
        r = rs_.find(id);
        if (!r) return -1;
    }
    stroking_ = true;
    const int W = r->maskW, H = r->maskH;
    auto disc = [&](double cx, double cy) {
        const int x0 = std::max(0, (int)std::floor(cx - rad)), x1 = std::min(W - 1, (int)std::ceil(cx + rad));
        const int y0 = std::max(0, (int)std::floor(cy - rad)), y1 = std::min(H - 1, (int)std::ceil(cy + rad));
        for (int yy = y0; yy <= y1; ++yy)
            for (int xx = x0; xx <= x1; ++xx)
                if ((xx + 0.5 - cx) * (xx + 0.5 - cx) + (yy + 0.5 - cy) * (yy + 0.5 - cy) <= rad * rad) r->mask[(size_t)yy * W + xx] = erase ? 0 : 1;
    };
    // a fast mouse jumps: paint discs along the segment from the previous point so the stroke stays continuous
    const QPointF to(x, y);
    const QPointF from = havePaint_ ? lastPaint_ : to;
    const double len = std::hypot(to.x() - from.x(), to.y() - from.y());
    const int n = std::max(1, (int)std::ceil(len / std::max(0.5, rad * 0.5)));
    for (int k = 1; k <= n; ++k) disc(from.x() + (to.x() - from.x()) * k / n, from.y() + (to.y() - from.y()) * k / n);
    lastPaint_ = to;
    havePaint_ = true;
    rs_.selected = id;
    rebuildOverlay();
    emit roisChanged();
    return id;
}

void RoiController::endStroke() {
    havePaint_ = false;
    if (!stroking_) return;
    stroking_ = false;
    // an erased-away mask disappears
    for (const Roi& r : rs_.rois)
        if (r.type == RoiType::Mask && std::find(r.mask.begin(), r.mask.end(), 1) == r.mask.end()) { rs_.remove(r.id); break; }
    edited(true);
}

void RoiController::edited(bool geometry) {
    rebuildOverlay();
    emit roisChanged();
    if (!stroking_) onRoisEdited(geometry);
}

void RoiController::setRois(std::vector<Roi> rois) {
    rs_.clear();
    rs_.nextId = 1;
    for (Roi& r : rois) rs_.add(std::move(r));
    edited(true);
}

void RoiController::rebuildOverlay() {
    const int W = sensorW(), H = sensorH();
    QImage img(W, H, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    decorateOverlay(img);
    bool any = false;
    for (const Roi& r : rs_.rois) {
        if (r.type != RoiType::Mask || r.maskW != W || r.maskH != H) continue;
        any = true;
        const int a = r.id == rs_.selected ? 120 : 80;
        const QColor c = r.color;
        const QRgb px = qPremultiply(qRgba(c.red(), c.green(), c.blue(), a));
        for (int y = 0; y < H; ++y) {
            QRgb* row = reinterpret_cast<QRgb*>(img.scanLine(y));
            const uint8_t* m = &r.mask[(size_t)y * W];
            for (int x = 0; x < W; ++x) if (m[x]) row[x] = px;
        }
    }
    (void)any;
    std::lock_guard<std::mutex> lk(ovmx_);
    ov_ = img;
    ++ovSeq_;
}

bool RoiController::takeOverlay(QImage& out, uint64_t& seq) {
    std::lock_guard<std::mutex> lk(ovmx_);
    if (seq == ovSeq_) return false;
    out = ov_;
    seq = ovSeq_;
    return true;
}

}  // namespace evs

#include "roi.hpp"
#include <QJsonArray>
#include <QStringList>
#include <algorithm>
#include <cmath>

namespace evs {

QString signal_name(SignalKind k) {
    switch (k) {
    case SignalKind::OnOff: return "onoff";
    case SignalKind::On: return "on";
    case SignalKind::Off: return "off";
    case SignalKind::Count: return "count";
    default: return "auto";
    }
}

SignalKind signal_from(const QString& s) {
    if (s == "onoff") return SignalKind::OnOff;
    if (s == "on") return SignalKind::On;
    if (s == "off") return SignalKind::Off;
    if (s == "count") return SignalKind::Count;
    return SignalKind::Auto;
}

static QString type_name(RoiType t) {
    switch (t) {
    case RoiType::Rect: return "rect";
    case RoiType::Polygon: return "polygon";
    case RoiType::Mask: return "mask";
    default: return "line";
    }
}

static RoiType type_from(const QString& s) {
    if (s == "rect") return RoiType::Rect;
    if (s == "polygon") return RoiType::Polygon;
    if (s == "mask") return RoiType::Mask;
    return RoiType::Line;
}

QColor roi_color(int i) {
    static const QColor c[] = {QColor("#4da3ff"), QColor("#ff9a47"), QColor("#7ed36b"), QColor("#c48cff"), QColor("#ffe066"),
                               QColor("#ff6b9a"), QColor("#2fd0d0"), QColor("#e0e0e0"), QColor("#a0e05a"), QColor("#ff7a6b")};
    return c[((i % 10) + 10) % 10];
}

double Roi::length() const {
    if (type == RoiType::Line && pts.size() >= 2) return std::hypot(pts[1].x() - pts[0].x(), pts[1].y() - pts[0].y());
    return 0;
}

QRectF Roi::bounds() const {
    if (type == RoiType::Mask) {
        int x0 = maskW, y0 = maskH, x1 = -1, y1 = -1;
        for (int y = 0; y < maskH; ++y)
            for (int x = 0; x < maskW; ++x)
                if (mask[(size_t)y * maskW + x]) { x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y); }
        return x1 < 0 ? QRectF() : QRectF(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
    }
    if (pts.empty()) return {};
    double x0 = pts[0].x(), x1 = x0, y0 = pts[0].y(), y1 = y0;
    for (const QPointF& p : pts) { x0 = std::min(x0, p.x()); x1 = std::max(x1, p.x()); y0 = std::min(y0, p.y()); y1 = std::max(y1, p.y()); }
    if (type == RoiType::Line) { x0 -= half; y0 -= half; x1 += half; y1 += half; }
    return QRectF(x0, y0, x1 - x0, y1 - y0);
}

RoiPixels rasterize(const Roi& r, int W, int H) {
    RoiPixels px;
    auto push = [&](int x, int y, int8_t s) { px.idx.push_back(uint32_t(y) * W + x); px.sign.push_back(s); };
    if (r.type == RoiType::Line) {
        if (r.pts.size() < 2) return px;
        const double x0 = r.pts[0].x(), y0 = r.pts[0].y(), L = r.length();
        if (L < 1) return px;
        const double ux = (r.pts[1].x() - x0) / L, uy = (r.pts[1].y() - y0) / L, nx = -uy, ny = ux;
        const QRectF b = r.bounds();
        const int xa = std::max(0, (int)std::floor(b.left() - 1)), xb = std::min(W - 1, (int)std::ceil(b.right() + 1));
        const int ya = std::max(0, (int)std::floor(b.top() - 1)), yb = std::min(H - 1, (int)std::ceil(b.bottom() + 1));
        const bool sided = r.sided();
        for (int y = ya; y <= yb; ++y)
            for (int x = xa; x <= xb; ++x) {
                const double dx = x + 0.5 - x0, dy = y + 0.5 - y0;
                const double s = dx * ux + dy * uy, d = dx * nx + dy * ny;
                if (s < 0 || s > L || std::fabs(d) > r.half) continue;
                push(x, y, sided ? (d >= 0 ? -1 : 1) : 1);
            }
    } else if (r.type == RoiType::Rect) {
        if (r.pts.size() < 2) return px;
        const int xa = std::clamp((int)std::floor(std::min(r.pts[0].x(), r.pts[1].x())), 0, W), xb = std::clamp((int)std::ceil(std::max(r.pts[0].x(), r.pts[1].x())), 0, W);
        const int ya = std::clamp((int)std::floor(std::min(r.pts[0].y(), r.pts[1].y())), 0, H), yb = std::clamp((int)std::ceil(std::max(r.pts[0].y(), r.pts[1].y())), 0, H);
        for (int y = ya; y < yb; ++y)
            for (int x = xa; x < xb; ++x) push(x, y, 1);
    } else if (r.type == RoiType::Polygon) {
        const size_t n = r.pts.size();
        if (n < 3) return px;
        const QRectF b = r.bounds();
        const int ya = std::max(0, (int)std::floor(b.top())), yb = std::min(H - 1, (int)std::ceil(b.bottom()));
        std::vector<double> xs;
        for (int y = ya; y <= yb; ++y) {   // scanline through pixel centres, even-odd rule
            const double yc = y + 0.5;
            xs.clear();
            for (size_t i = 0; i < n; ++i) {
                const QPointF& a = r.pts[i];
                const QPointF& c = r.pts[(i + 1) % n];
                if ((a.y() <= yc) == (c.y() <= yc)) continue;
                xs.push_back(a.x() + (yc - a.y()) / (c.y() - a.y()) * (c.x() - a.x()));
            }
            std::sort(xs.begin(), xs.end());
            for (size_t k = 0; k + 1 < xs.size(); k += 2) {
                const int xa = std::max(0, (int)std::ceil(xs[k] - 0.5)), xb = std::min(W - 1, (int)std::floor(xs[k + 1] - 0.5));
                for (int x = xa; x <= xb; ++x) push(x, y, 1);
            }
        }
    } else {
        if (r.maskW != W || r.maskH != H || r.mask.size() != (size_t)W * H) return px;
        for (size_t i = 0; i < r.mask.size(); ++i)
            if (r.mask[i]) { px.idx.push_back((uint32_t)i); px.sign.push_back(1); }
    }
    return px;
}

void build_lut(const std::vector<Roi>& rois, int W, int H, std::vector<uint16_t>& lut, std::vector<int8_t>& sign) {
    lut.assign((size_t)W * H, 0);
    sign.assign((size_t)W * H, 0);
    for (size_t k = 0; k < rois.size() && k < 65000; ++k) {
        const RoiPixels p = rasterize(rois[k], W, H);
        for (size_t i = 0; i < p.idx.size(); ++i) {
            if (lut[p.idx[i]]) continue;
            lut[p.idx[i]] = uint16_t(k + 1);
            sign[p.idx[i]] = p.sign[i];
        }
    }
}

QString mask_rle(const std::vector<uint8_t>& m) {
    QStringList parts;
    size_t i = 0;
    while (i < m.size()) {
        if (!m[i]) { ++i; continue; }
        size_t j = i;
        while (j < m.size() && m[j]) ++j;
        parts << QString("%1:%2").arg(i).arg(j - i);
        i = j;
    }
    return parts.join(',');
}

std::vector<uint8_t> mask_from_rle(const QString& s, size_t n) {
    std::vector<uint8_t> m(n, 0);
    for (const QString& part : s.split(',', Qt::SkipEmptyParts)) {
        const QStringList kv = part.split(':');
        if (kv.size() != 2) continue;
        const qulonglong a = kv[0].toULongLong(), len = kv[1].toULongLong();
        for (qulonglong i = a; i < a + len && i < n; ++i) m[i] = 1;
    }
    return m;
}

QJsonObject roi_to_json(const Roi& r) {
    QJsonObject o{{"id", r.id}, {"name", r.name}, {"color", r.color.name()}, {"type", type_name(r.type)}, {"signal", signal_name(r.signal)}};
    QJsonArray pa;
    for (const QPointF& p : r.pts) pa.append(QJsonArray{std::round(p.x() * 100) / 100, std::round(p.y() * 100) / 100});
    o["pts"] = pa;
    if (r.type == RoiType::Line) o["half"] = r.half;
    if (r.type == RoiType::Mask) { o["mask_rle"] = mask_rle(r.mask); o["mask_w"] = r.maskW; o["mask_h"] = r.maskH; }
    if (!r.source.isEmpty()) o["source"] = r.source;
    return o;
}

Roi roi_from_json(const QJsonObject& o, int W, int H) {
    Roi r;
    r.id = o["id"].toInt();
    r.name = o["name"].toString();
    r.color = QColor(o["color"].toString());
    r.type = type_from(o["type"].toString());
    r.signal = signal_from(o["signal"].toString());
    for (const QJsonValue& v : o["pts"].toArray()) {
        const QJsonArray a = v.toArray();
        if (a.size() >= 2) r.pts.emplace_back(a[0].toDouble(), a[1].toDouble());
    }
    r.half = o["half"].toDouble(6);
    r.source = o["source"].toString();
    if (r.type == RoiType::Mask) {
        r.maskW = o["mask_w"].toInt(W);
        r.maskH = o["mask_h"].toInt(H);
        r.mask = mask_from_rle(o["mask_rle"].toString(), (size_t)r.maskW * r.maskH);
    }
    return r;
}

QVariantMap roi_to_variant(const Roi& r) {
    QVariantList pts;
    for (const QPointF& p : r.pts) pts << QVariantMap{{"x", p.x()}, {"y", p.y()}};
    const QRectF b = r.bounds();
    return QVariantMap{{"id", r.id}, {"name", r.name}, {"color", r.color}, {"type", type_name(r.type)}, {"signal", signal_name(r.signal)},
                       {"pts", pts}, {"half", r.half}, {"len", r.length()}, {"source", r.source},
                       {"bx", b.x()}, {"by", b.y()}, {"bw", b.width()}, {"bh", b.height()}};
}

int RoiSet::add(Roi r) {
    if (r.id <= 0 || find(r.id)) r.id = nextId;
    nextId = std::max(nextId, r.id + 1);
    if (!r.color.isValid()) r.color = roi_color(r.id - 1);
    if (r.name.isEmpty()) {
        const char* pre = r.type == RoiType::Line ? "L" : r.type == RoiType::Rect ? "R" : r.type == RoiType::Polygon ? "P" : "M";
        r.name = QString("%1%2").arg(pre).arg(r.id);
    }
    rois.push_back(std::move(r));
    return rois.back().id;
}

Roi* RoiSet::find(int id) {
    for (Roi& r : rois) if (r.id == id) return &r;
    return nullptr;
}
const Roi* RoiSet::find(int id) const {
    for (const Roi& r : rois) if (r.id == id) return &r;
    return nullptr;
}
int RoiSet::indexOf(int id) const {
    for (size_t i = 0; i < rois.size(); ++i) if (rois[i].id == id) return (int)i;
    return -1;
}
bool RoiSet::remove(int id) {
    const int i = indexOf(id);
    if (i < 0) return false;
    rois.erase(rois.begin() + i);
    if (selected == id) selected = -1;
    return true;
}

QJsonArray RoiSet::toJson() const {
    QJsonArray a;
    for (const Roi& r : rois) a.append(roi_to_json(r));
    return a;
}

void RoiSet::fromJson(const QJsonArray& a, int W, int H) {
    clear();
    nextId = 1;
    for (const QJsonValue& v : a) add(roi_from_json(v.toObject(), W, H));
}

}  // namespace evs

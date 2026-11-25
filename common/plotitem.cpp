#include "plotitem.hpp"
#include <QDataStream>
#include <QPainter>
#include <QQuickWindow>
#include <QSGClipNode>
#include <QSGGeometryNode>
#include <QSGImageNode>
#include <QSGVertexColorMaterial>
#include <QSGRendererInterface>
#include <algorithm>
#include "prof.hpp"

namespace hz {

QRgb viridis(float t) {
    static const float k[9][3] = {{0.267f, 0.005f, 0.329f}, {0.283f, 0.141f, 0.458f}, {0.254f, 0.265f, 0.530f},
                                  {0.207f, 0.372f, 0.553f}, {0.164f, 0.471f, 0.558f}, {0.128f, 0.567f, 0.551f},
                                  {0.135f, 0.659f, 0.518f}, {0.478f, 0.821f, 0.318f}, {0.993f, 0.906f, 0.144f}};
    t = std::clamp(t, 0.0f, 1.0f) * 8;
    int i = std::min(7, (int)t);
    float f = t - i;
    auto ch = [&](int c) { return (int)std::lround(255 * (k[i][c] + f * (k[i + 1][c] - k[i][c]))); };
    return qRgb(ch(0), ch(1), ch(2));
}

static const QColor C_TEXT(0x9b, 0xa1, 0xab), C_GRID(255, 255, 255, 16), C_AXIS(255, 255, 255, 40);

static const QFont& plotFont(int px) {
    static QFont f11 = [] { QFont f("Segoe UI Variable Text"); f.setPixelSize(11); return f; }();
    static QFont f10 = [] { QFont f("Segoe UI Variable Text"); f.setPixelSize(10); return f; }();
    return px == 10 ? f10 : f11;
}

PlotItem::PlotItem(QQuickItem* parent) : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
    setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton);
    setAcceptHoverEvents(true);
}

static std::vector<PlotItem*>& registry() { static std::vector<PlotItem*> r; return r; }

PlotItem::~PlotItem() {
    auto& r = registry();
    r.erase(std::remove(r.begin(), r.end(), this), r.end());
}

void PlotItem::redraw(bool geometry) {
    if (geometry) mesh_dirty_ = true;
    polish();
    update();
}

void PlotItem::setSyncGroup(const QString& g) {
    if (g == group_) return;
    auto& r = registry();
    r.erase(std::remove(r.begin(), r.end(), this), r.end());
    group_ = g;
    if (!g.isEmpty()) { r.push_back(this); adoptView(); redraw(true); }
    emit syncChanged();
}

void PlotItem::setSyncY(bool v) {
    if (v == sync_y_) return;
    sync_y_ = v;
    if (!v) { user_y_ = false; autoRange(); redraw(true); }
    emit syncChanged();
}

std::vector<PlotItem*> PlotItem::peers() const {
    std::vector<PlotItem*> out;
    if (group_.isEmpty()) return out;
    for (PlotItem* o : registry())
        if (o != this && o->group_ == group_ && o->d_.logX == d_.logX) out.push_back(o);
    return out;
}

void PlotItem::pushView(bool reset) {
    for (PlotItem* o : peers()) {
        if (reset) { o->user_x_ = false; if (sync_y_ && o->sync_y_) o->user_y_ = false; o->autoRange(); }
        else {
            o->vx0_ = vx0_; o->vx1_ = vx1_; o->user_x_ = user_x_;
            if (sync_y_ && o->sync_y_ && o->d_.logY == d_.logY) { o->vy0_ = vy0_; o->vy1_ = vy1_; o->user_y_ = user_y_; }
        }
        o->redraw(true);
    }
}

void PlotItem::adoptView() {
    for (PlotItem* o : peers()) {
        if (o->user_x_ && !user_x_) { vx0_ = o->vx0_; vx1_ = o->vx1_; user_x_ = true; }
        if (sync_y_ && o->sync_y_ && o->user_y_ && !user_y_ && o->d_.logY == d_.logY) { vy0_ = o->vy0_; vy1_ = o->vy1_; user_y_ = true; }
        if (user_x_) break;
    }
}

void PlotItem::pushHover() {
    double X = area().contains(hover_) ? ix(hover_.x()) : NAN;
    for (PlotItem* o : peers()) { o->peer_hover_ = X; o->redraw(false); }
}

QObject* PlotItem::source() const { return src_; }

void PlotItem::setSource(QObject* s) {
    auto* c = qobject_cast<PlotSource*>(s);
    if (c == src_) return;
    if (src_) disconnect(src_, nullptr, this, nullptr);
    src_ = c;
    if (src_) connect(src_, &PlotSource::plotsChanged, this, &PlotItem::refresh);
    emit sourceChanged();
    refresh();
}

void PlotItem::setKind(const QString& k) {
    if (k == kind_) return;
    kind_ = k;
    user_x_ = user_y_ = false;
    emit kindChanged();
    refresh();
}

void PlotItem::setCursorX(double v) {
    if (v == cursor_ || (std::isnan(v) && std::isnan(cursor_))) return;
    cursor_ = v;
    emit cursorXChanged();
    redraw(false);
}

void PlotItem::refresh() {
    if (!src_ || kind_.isEmpty()) return;
    // a hidden plot (other page, collapsed card) costs nothing: it fetches its data when it is shown again
    if (!isVisible() || width() <= 0 || height() <= 0) { stale_ = true; return; }
    stale_ = false;
    ProfScope prof_("plot.refresh");
    PlotData nd;
    src_->fillPlot(kind_, nd);
    const bool had = hasData();
    bool fix_changed = nd.fixX != d_.fixX || nd.logY != d_.logY || nd.logX != d_.logX;
    d_ = std::move(nd);
    if (fix_changed) user_x_ = user_y_ = false;
    autoRange();
    adoptView();
    if (had != hasData()) emit dataChanged();
    redraw(true);
}

void PlotItem::itemChange(ItemChange c, const ItemChangeData& v) {
    QQuickItem::itemChange(c, v);
    if (c == ItemVisibleHasChanged && v.boolValue && stale_) refresh();
    else if (c == ItemDevicePixelRatioHasChanged || c == ItemSceneChange) redraw(true);
}

void PlotItem::geometryChange(const QRectF& n, const QRectF& o) {
    QQuickItem::geometryChange(n, o);
    if (n.size() == o.size()) return;
    if (stale_ && n.width() > 0 && n.height() > 0) refresh();
    else redraw(true);
}

void PlotItem::resetView() {
    user_x_ = user_y_ = false;
    autoRange();
    pushView(true);
    redraw(true);
}

void PlotItem::autoRange() {
    auto fx = [&](double v) { return d_.logX ? std::log10(std::max(v, 1e-30)) : v; };
    auto fy = [&](double v) { return d_.logY ? std::log10(std::max(v, 1e-30)) : v; };
    double x0 = 1e300, x1 = -1e300, y0 = 1e300, y1 = -1e300;
    for (const auto& s : d_.series) {
        const bool needX = !d_.fixX || s.kind == PlotSeries::Bars, needY = !d_.fixY;
        if (!needX && !needY) continue;   // fixed axes: no need to scan the data
        for (size_t k = 0; k < s.x.size() && k < s.y.size(); ++k) {
            if (!std::isfinite(s.x[k]) || !std::isfinite(s.y[k])) continue;
            if (d_.logY && s.y[k] <= 0) continue;
            double X = fx(s.x[k]), Y = fy(s.y[k]);
            if (s.kind == PlotSeries::Bars) { x0 = std::min(x0, X - s.barWidth / 2); x1 = std::max(x1, X + s.barWidth / 2); y0 = std::min(y0, 0.0); }
            else { x0 = std::min(x0, X); x1 = std::max(x1, X); }
            y0 = std::min(y0, Y); y1 = std::max(y1, Y);
        }
    }
    if (d_.heat.valid) {
        x0 = std::min(x0, d_.heat.x0); x1 = std::max(x1, d_.heat.x1);
        y0 = std::min(y0, std::min(d_.heat.y0, d_.heat.y1)); y1 = std::max(y1, std::max(d_.heat.y0, d_.heat.y1));
    }
    if (x0 > x1) { x0 = 0; x1 = 1; }
    if (y0 > y1) { y0 = 0; y1 = 1; }
    if (d_.fixX) { x0 = fx(d_.x0); x1 = fx(d_.x1); }
    if (d_.fixY) { y0 = fy(d_.y0); y1 = fy(d_.y1); }
    else {
        if (d_.yFromZero && !d_.logY) y0 = std::min(0.0, y0);
        double pad = (y1 - y0) * 0.06;
        if (pad <= 0) pad = std::max(std::abs(y1) * 0.1, 1e-12);
        if (!d_.heat.valid) { y1 += pad; if (!(d_.yFromZero && y0 == 0)) y0 -= pad; }
        if (d_.logY) y0 = std::max(y0, y1 - 5);
    }
    if (x1 - x0 <= 0) { x0 -= 0.5; x1 += 0.5; }
    if (!user_x_) { vx0_ = x0; vx1_ = x1; }
    if (!user_y_) { vy0_ = y0; vy1_ = y1; }
}

QRectF PlotItem::area() const {
    // axis titles only when the plot is tall enough: on short plots they get clipped and steal the data's room
    double l = showYLabel() ? 60 : 46, b = showXLabel() ? 40 : 24;
    return QRectF(l, 10, std::max(10.0, width() - l - 12), std::max(10.0, height() - 10 - b));
}
double PlotItem::tx(double x) const { QRectF a = area(); return a.left() + (x - vx0_) / (vx1_ - vx0_) * a.width(); }
double PlotItem::ty(double y) const { QRectF a = area(); return a.bottom() - (y - vy0_) / (vy1_ - vy0_) * a.height(); }
double PlotItem::ix(double px) const { QRectF a = area(); return vx0_ + (px - a.left()) / a.width() * (vx1_ - vx0_); }
double PlotItem::iy(double py) const { QRectF a = area(); return vy0_ + (a.bottom() - py) / a.height() * (vy1_ - vy0_); }

static std::vector<double> ticks(double a, double b, int target, double& step) {
    double range = b - a;
    std::vector<double> t;
    if (!(range > 0)) { step = 1; return t; }
    double raw = range / std::max(target, 2), mag = std::pow(10, std::floor(std::log10(raw))), r = raw / mag;
    step = (r < 1.5 ? 1 : r < 3 ? 2 : r < 7 ? 5 : 10) * mag;
    for (double v = std::ceil(a / step) * step; v <= b + step * 1e-9; v += step) t.push_back(std::abs(v) < step * 1e-9 ? 0 : v);
    return t;
}

static QString num(double v, double step) {
    double a = std::abs(v);
    if (a < std::abs(step) * 1e-6) return "0";
    if (a != 0 && (a >= 1e5 || a < 1e-3)) return QString::number(v, 'g', 3);
    int dec = step >= 1 ? 0 : std::min(6, (int)std::ceil(-std::log10(step) - 1e-9));
    return QString::number(v, 'f', dec);
}

// ---- cached QPainter layers ---------------------------------------------------------------------------------------------
QImage PlotItem::layerImage() const {
    const qreal dpr = window() ? window()->effectiveDevicePixelRatio() : 1.0;
    QImage img(std::max(1, (int)std::ceil(width() * dpr)), std::max(1, (int)std::ceil(height() * dpr)), QImage::Format_ARGB32_Premultiplied);
    img.setDevicePixelRatio(dpr);
    img.fill(Qt::transparent);
    return img;
}

// everything the back layer shows; it is repainted only when this changes
QByteArray PlotItem::backKey() const {
    QByteArray k;
    QDataStream s(&k, QIODevice::WriteOnly);
    s << width() << height() << (window() ? window()->effectiveDevicePixelRatio() : 1.0) << vx0_ << vx1_ << vy0_ << vy1_ << d_.logX << d_.logY << d_.xlabel << d_.ylabel;
    return k;
}

QByteArray PlotItem::frontKey() const {
    QByteArray k;
    QDataStream s(&k, QIODevice::WriteOnly);
    s << width() << height() << (window() ? window()->effectiveDevicePixelRatio() : 1.0) << vx0_ << vx1_ << vy0_ << vy1_ << d_.logX << d_.logY;
    s << hasData() << d_.empty << d_.legend << cursor_ << peer_hover_;
    const bool hov = area().contains(hover_) && hasData();
    s << hov;
    if (hov) s << hover_ << d_.xunit << d_.yunit;
    for (const auto& m : d_.marks) s << m.v << m.color << m.width << m.dashed << m.label << m.horizontal;
    for (const auto& t : d_.texts) s << t.x << t.y << t.text << t.color;
    for (const auto& se : d_.series) {
        s << (int)se.kind << se.name << se.color << se.legend << se.style;
        if (se.kind == PlotSeries::Scatter) {
            s << se.marker << se.size << se.hollow;
            s.writeRawData(reinterpret_cast<const char*>(se.x.data()), int(se.x.size() * sizeof(double)));
            s.writeRawData(reinterpret_cast<const char*>(se.y.data()), int(se.y.size() * sizeof(double)));
        }
    }
    return k;
}

void PlotItem::paintBack(QPainter* p) {
    p->setRenderHint(QPainter::Antialiasing, true);
    p->setRenderHint(QPainter::TextAntialiasing, true);
    const QFont& f = plotFont(11);
    p->setFont(f);
    QRectF A = area();
    double xs, ys;
    auto xt = ticks(vx0_, vx1_, std::max(3, (int)(A.width() / 90)), xs);
    auto yt = d_.logY ? std::vector<double>() : ticks(vy0_, vy1_, std::max(3, (int)(A.height() / 44)), ys);
    if (d_.logY) { ys = 1; for (double v = std::ceil(vy0_); v <= vy1_; v += 1) yt.push_back(v); }
    p->setPen(QPen(C_GRID, 1));
    for (double v : xt) p->drawLine(QPointF(tx(v), A.top()), QPointF(tx(v), A.bottom()));
    for (double v : yt) p->drawLine(QPointF(A.left(), ty(v)), QPointF(A.right(), ty(v)));
    p->setPen(C_TEXT);
    for (double v : xt) {
        QString s = d_.logX ? QString("1e%1").arg(v, 0, 'f', 0) : num(v, xs);
        p->drawText(QRectF(tx(v) - 40, A.bottom() + 4, 80, 14), Qt::AlignHCenter | Qt::AlignTop, s);
    }
    for (double v : yt) {
        QString s = d_.logY ? QString("1e%1").arg(v, 0, 'f', 0) : num(v, ys);
        p->drawText(QRectF(0, ty(v) - 7, A.left() - 6, 14), Qt::AlignRight | Qt::AlignVCenter, s);
    }
    if (showXLabel()) p->drawText(QRectF(A.left(), A.bottom() + 20, A.width(), 16), Qt::AlignHCenter | Qt::AlignTop, d_.xlabel);
    if (showYLabel()) {
        p->save();
        p->translate(12, A.center().y());
        p->rotate(-90);
        p->drawText(QRectF(-A.height() / 2, -8, A.height(), 16), Qt::AlignCenter, d_.ylabel);
        p->restore();
    }
    p->setPen(QPen(C_AXIS, 1));
    p->drawLine(A.bottomLeft(), A.bottomRight());
}

void PlotItem::paintFront(QPainter* p) {
    p->setRenderHint(QPainter::Antialiasing, true);
    p->setRenderHint(QPainter::TextAntialiasing, true);
    const QFont& f = plotFont(11);
    p->setFont(f);
    QFontMetricsF fm(f);
    QRectF A = area();
    auto fx = [&](double v) { return d_.logX ? std::log10(std::max(v, 1e-30)) : v; };
    auto fy = [&](double v) { return d_.logY ? std::log10(std::max(v, 1e-30)) : v; };

    p->save();
    p->setClipRect(A.adjusted(-1, -1, 1, 1));
    // scatter markers (lines and bars are meshes)
    for (const auto& s : d_.series) {
        if (s.kind != PlotSeries::Scatter) continue;
        const size_t n = std::min(s.x.size(), s.y.size());
        for (size_t k = 0; k < n; ++k) {
            if (!std::isfinite(s.y[k]) || !std::isfinite(s.x[k])) continue;
            QPointF q(tx(fx(s.x[k])), ty(fy(s.y[k])));
            float r = s.size / 2;
            if (s.hollow) { p->setPen(QPen(s.color, 1.6)); p->setBrush(Qt::NoBrush); }
            else { p->setPen(QPen(s.color.darker(140), 1)); p->setBrush(s.color); }
            if (s.marker == 1) p->drawRect(QRectF(q.x() - r, q.y() - r, 2 * r, 2 * r));
            else if (s.marker == 2) {
                QPolygonF poly({QPointF(q.x(), q.y() - r * 1.3), QPointF(q.x() + r * 1.3, q.y()), QPointF(q.x(), q.y() + r * 1.3), QPointF(q.x() - r * 1.3, q.y())});
                p->drawPolygon(poly);
            } else p->drawEllipse(q, r, r);
        }
    }
    // marker lines
    for (const auto& m : d_.marks) {
        QPen pen(m.color, m.width, m.dashed ? Qt::DashLine : Qt::SolidLine);
        p->setPen(pen);
        if (m.horizontal) { double y = ty(fy(m.v)); p->drawLine(QPointF(A.left(), y), QPointF(A.right(), y)); }
        else {
            double x = tx(fx(m.v));
            p->drawLine(QPointF(x, A.top()), QPointF(x, A.bottom()));
            if (!m.label.isEmpty()) {
                QRectF r(x + 4, A.top() + 2, fm.horizontalAdvance(m.label) + 8, 16);
                if (r.right() > A.right()) r.moveRight(x - 4);
                p->fillRect(r, QColor(20, 22, 26, 210));
                p->setPen(m.color);
                p->drawText(r, Qt::AlignCenter, m.label);
            }
        }
    }
    if (std::isfinite(cursor_)) {
        p->setPen(QPen(QColor(255, 255, 255, 150), 1, Qt::DashLine));
        double x = tx(fx(cursor_));
        p->drawLine(QPointF(x, A.top()), QPointF(x, A.bottom()));
    }
    if (std::isfinite(peer_hover_) && !A.contains(hover_)) {
        p->setPen(QPen(QColor(255, 255, 255, 45), 1));
        double x = tx(peer_hover_);
        p->drawLine(QPointF(x, A.top()), QPointF(x, A.bottom()));
    }
    // text labels (skip overlapping ones)
    std::vector<QRectF> used;
    const QFont& fs = plotFont(10);
    p->setFont(fs);
    QFontMetricsF fms(fs);
    for (const auto& t : d_.texts) {
        if (!std::isfinite(t.y) || (d_.logY && t.y <= 0)) continue;
        QStringList lines = t.text.split('\n');
        double w = 0;
        for (auto& l : lines) w = std::max(w, fms.horizontalAdvance(l));
        double h = lines.size() * 12.0;
        QRectF r(tx(fx(t.x)) - w / 2 - 3, ty(fy(t.y)) - h - 8, w + 6, h + 2);
        bool clash = false;
        for (auto& u : used) if (u.intersects(r)) { clash = true; break; }
        if (clash || !A.contains(r.center())) continue;
        used.push_back(r);
        p->fillRect(r, QColor(20, 22, 26, 170));
        p->setPen(t.color);
        p->drawText(r, Qt::AlignCenter, t.text);
    }
    p->setFont(f);
    p->restore();

    // empty state
    if (!hasData()) {
        p->setPen(C_TEXT);
        p->drawText(A, Qt::AlignCenter | Qt::TextWordWrap, d_.empty);
    }
    // legend
    if (d_.legend) {
        std::vector<const PlotSeries*> ls;
        for (const auto& s : d_.series) if (s.legend && !s.name.isEmpty()) ls.push_back(&s);
        if (!ls.empty()) {
            double w = 0;
            for (auto* s : ls) w = std::max(w, fm.horizontalAdvance(s->name));
            int per_col = std::max(1, (int)((A.height() - 16) / 16));
            int ncol = ((int)ls.size() + per_col - 1) / per_col;
            int rows = std::min((int)ls.size(), per_col);
            QRectF box(A.right() - ncol * (w + 26) - 8, A.top() + 6, ncol * (w + 26) + 4, rows * 16 + 6);
            p->setPen(Qt::NoPen);
            p->setBrush(QColor(22, 24, 28, 215));
            p->drawRoundedRect(box, 6, 6);
            for (size_t k = 0; k < ls.size(); ++k) {
                int col = (int)k / per_col, row = (int)k % per_col;
                QPointF o(box.left() + 8 + col * (w + 26), box.top() + 5 + row * 16);
                p->setBrush(ls[k]->color);
                if (ls[k]->kind == PlotSeries::Line && ls[k]->style != 0) {
                    p->setPen(QPen(ls[k]->color, 2, ls[k]->style == 1 ? Qt::DashLine : ls[k]->style == 2 ? Qt::DotLine : ls[k]->style == 3 ? Qt::DashDotLine : Qt::DashDotDotLine, Qt::FlatCap));
                    p->drawLine(QPointF(o.x(), o.y() + 7.5), QPointF(o.x() + 13, o.y() + 7.5));
                } else if (ls[k]->kind == PlotSeries::Line) p->drawRoundedRect(QRectF(o.x(), o.y() + 6, 12, 3), 1.5, 1.5);
                else p->drawEllipse(QRectF(o.x() + 2, o.y() + 3, 8, 8));
                p->setPen(QColor(0xd8, 0xdc, 0xe2));
                p->drawText(QRectF(o.x() + 17, o.y(), w + 4, 14), Qt::AlignLeft | Qt::AlignVCenter, ls[k]->name);
                p->setPen(Qt::NoPen);
            }
        }
    }
    // hover readout
    if (A.contains(hover_) && hasData()) {
        p->setPen(QPen(QColor(255, 255, 255, 60), 1));
        p->drawLine(QPointF(hover_.x(), A.top()), QPointF(hover_.x(), A.bottom()));
        double X = ix(hover_.x()), Y = iy(hover_.y());
        if (d_.logX) X = std::pow(10, X);
        if (d_.logY) Y = std::pow(10, Y);
        double xs2 = (vx1_ - vx0_) / 500, ys2 = (vy1_ - vy0_) / 300;
        QString s = num(X, d_.logX ? X / 100 : xs2) + (d_.xunit.isEmpty() ? "" : " " + d_.xunit) + "   " +
                    (d_.heat.valid ? QString() : num(Y, d_.logY ? std::abs(Y) / 100 : ys2) + (d_.yunit.isEmpty() ? "" : " " + d_.yunit));
        QRectF r(A.left() + 6, A.top() + 4, fm.horizontalAdvance(s) + 12, 18);
        p->setPen(Qt::NoPen);
        p->setBrush(QColor(22, 24, 28, 225));
        p->drawRoundedRect(r, 5, 5);
        p->setPen(QColor(0xe6, 0xe8, 0xeb));
        p->drawText(r, Qt::AlignCenter, s);
    }
}

// ---- line / bar meshes ------------------------------------------------------------------------------------------------------
// An anti-aliased line is a strip of "stations": per point four vertices across the line (outer fringe alpha 0, core edge,
// core edge, outer fringe alpha 0); neighbouring stations are joined by three quads. The coverage across the line is
// 2 * core + 1 px, so core = width / 2 - 0.5 gives the requested width.
namespace {
using V = QSGGeometry::ColoredPoint2D;
struct Rgba { float r, g, b, a; };

inline void vtx(std::vector<V>& v, float x, float y, const Rgba& c, float alpha) {
    const float a = c.a * alpha;   // premultiplied
    V p;
    p.set(x, y, (uchar)std::lround(c.r * a), (uchar)std::lround(c.g * a), (uchar)std::lround(c.b * a), (uchar)std::lround(255 * a));
    v.push_back(p);
}

// station at p, across direction d (|d| includes the miter factor), core half-width hc; joined to the previous station
inline void station(std::vector<V>& v, std::vector<quint32>& idx, float px, float py, float dx, float dy, float hc, const Rgba& c, bool join) {
    const quint32 b = (quint32)v.size();
    const float o = hc + 1.f;
    vtx(v, px + dx * o, py + dy * o, c, 0.f);
    vtx(v, px + dx * hc, py + dy * hc, c, 1.f);
    vtx(v, px - dx * hc, py - dy * hc, c, 1.f);
    vtx(v, px - dx * o, py - dy * o, c, 0.f);
    if (!join || b < 4) return;
    const quint32 a = b - 4;
    for (quint32 q = 0; q < 3; ++q) {
        const quint32 i0 = a + q, i1 = a + q + 1, j0 = b + q, j1 = b + q + 1;
        idx.insert(idx.end(), {i0, i1, j0, i1, j1, j0});
    }
}

// one connected polyline (item pixels)
void polyline(std::vector<V>& v, std::vector<quint32>& idx, const std::vector<QPointF>& p, float width, const Rgba& c) {
    const size_t n = p.size();
    if (n < 2) return;
    const float hc = std::max(0.f, width / 2 - 0.5f);
    auto segN = [&](size_t j, float& nx, float& ny) {   // unit normal of segment j -> j+1
        const double dx = p[j + 1].x() - p[j].x(), dy = p[j + 1].y() - p[j].y(), L = std::hypot(dx, dy);
        if (L < 1e-9) { nx = 0; ny = 1; return; }
        nx = float(-dy / L); ny = float(dx / L);
    };
    float pnx = 0, pny = 1;
    segN(0, pnx, pny);
    for (size_t i = 0; i < n; ++i) {
        float nx, ny;
        if (i == 0) { nx = pnx; ny = pny; }
        else if (i == n - 1) { nx = pnx; ny = pny; }
        else {
            float qnx, qny;
            segN(i, qnx, qny);
            float ax = pnx + qnx, ay = pny + qny;
            const float L = std::hypot(ax, ay);
            if (L < 1e-3f) { nx = qnx; ny = qny; }
            else {
                ax /= L; ay /= L;
                const float m = 1.f / std::max(0.5f, ax * qnx + ay * qny);   // miter, at most 2x
                nx = ax * m; ny = ay * m;
            }
            pnx = qnx; pny = qny;
        }
        station(v, idx, (float)p[i].x(), (float)p[i].y(), nx, ny, hc, c, i > 0);
    }
}

// dashed: split into "on" pieces along the arc length (pattern in units of the line width, like QPen)
void dashed(std::vector<V>& v, std::vector<quint32>& idx, const std::vector<QPointF>& p, float width, const Rgba& c, int style) {
    std::vector<float> pat;
    switch (style) {
    case 1: pat = {4, 2}; break;
    case 2: pat = {1, 2}; break;
    case 3: pat = {4, 2, 1, 2}; break;
    default: pat = {4, 2, 1, 2, 1, 2}; break;
    }
    const float u = std::max(1.f, width);
    for (float& x : pat) x *= u;
    size_t k = 0;
    float left = pat[0];
    bool on = true;
    std::vector<QPointF> run{p.empty() ? QPointF() : p[0]};
    for (size_t i = 0; i + 1 < p.size(); ++i) {
        QPointF a = p[i];
        const QPointF b = p[i + 1];
        double L = std::hypot(b.x() - a.x(), b.y() - a.y());
        while (L > 0) {
            if (L < left) { left -= (float)L; if (on) run.push_back(b); break; }
            const QPointF m = a + (b - a) * (left / L);
            if (on) { run.push_back(m); polyline(v, idx, run, width, c); run.clear(); }
            else run = {m};
            L -= left;
            a = m;
            on = !on;
            k = (k + 1) % pat.size();
            left = pat[k];
        }
    }
    if (on) polyline(v, idx, run, width, c);
}
}  // namespace

void PlotItem::buildMesh() {
    mesh_.v.clear();
    mesh_.i.clear();
    const QRectF A = area();
    auto fx = [&](double v) { return d_.logX ? std::log10(std::max(v, 1e-30)) : v; };
    auto fy = [&](double v) { return d_.logY ? std::log10(std::max(v, 1e-30)) : v; };
    const double sx = A.width() / (vx1_ - vx0_), sy = A.height() / (vy1_ - vy0_);
    const double X0 = A.left(), Y0 = A.bottom();
    auto PX = [&](double x) { return X0 + (fx(x) - vx0_) * sx; };
    auto PY = [&](double y) { return Y0 - (fy(y) - vy0_) * sy; };
    const int cols = std::max(1, (int)A.width());
    std::vector<QPointF> pts;
    for (const auto& s : d_.series) {
        const size_t n = std::min(s.x.size(), s.y.size());
        const Rgba c{(float)s.color.red(), (float)s.color.green(), (float)s.color.blue(), (float)s.color.alphaF()};
        if (s.kind == PlotSeries::Bars) {
            const double base = PY(d_.logY ? std::pow(10.0, vy0_) : 0.0);
            for (size_t k = 0; k < n; ++k) {
                if (!std::isfinite(s.y[k]) || !std::isfinite(s.x[k])) continue;
                const float xa = (float)(X0 + (s.x[k] - s.barWidth / 2 - vx0_) * sx), xb = (float)(X0 + (s.x[k] + s.barWidth / 2 - vx0_) * sx);
                const float ya = (float)PY(s.y[k]), yb = (float)base;
                const quint32 b = (quint32)mesh_.v.size();
                vtx(mesh_.v, xa, ya, c, 1); vtx(mesh_.v, xb, ya, c, 1); vtx(mesh_.v, xa, yb, c, 1); vtx(mesh_.v, xb, yb, c, 1);
                mesh_.i.insert(mesh_.i.end(), {b, b + 1, b + 2, b + 1, b + 3, b + 2});
            }
            continue;
        }
        if (s.kind != PlotSeries::Line || n < 2) continue;
        // points that matter: inside the visible x range, plus the neighbours that lead into it
        const double xl = A.left() - 1, xr = A.right() + 1;
        size_t visible = 0;
        std::vector<double> X(n);
        for (size_t k = 0; k < n; ++k) {
            X[k] = std::isfinite(s.x[k]) ? PX(s.x[k]) : NAN;
            if (X[k] >= xl && X[k] <= xr) ++visible;
        }
        if (visible > (size_t)2 * cols && s.style == 0) {
            // dense: min/max per pixel column, drawn as a filled band between the column extremes
            std::vector<float> lo(cols, 1e30f), hi(cols, -1e30f);
            for (size_t k = 0; k < n; ++k) {
                if (!std::isfinite(s.y[k]) || (d_.logY && s.y[k] <= 0)) continue;
                const int ci = (int)(X[k] - A.left());
                if (ci < 0 || ci >= cols) continue;
                const float Y = (float)PY(s.y[k]);
                lo[ci] = std::min(lo[ci], Y);
                hi[ci] = std::max(hi[ci], Y);
            }
            const float hw = std::max(0.f, s.width / 2 - 0.5f);
            bool first = true;
            for (int ci = 0; ci < cols; ++ci) {
                if (lo[ci] > hi[ci]) continue;
                const float xc = (float)(A.left() + ci + 0.5), yc = (lo[ci] + hi[ci]) / 2, half = (hi[ci] - lo[ci]) / 2 + hw;
                station(mesh_.v, mesh_.i, xc, yc, 0.f, 1.f, half, c, !first);
                first = false;
            }
            continue;
        }
        // sparse: anti-aliased polyline; NaN / non-positive (log) values break it
        pts.clear();
        auto flush = [&] {
            if (pts.size() >= 2) {
                if (s.style == 0) polyline(mesh_.v, mesh_.i, pts, s.width, c);
                else dashed(mesh_.v, mesh_.i, pts, s.width, c, s.style);
            }
            pts.clear();
        };
        for (size_t k = 0; k < n; ++k) {
            const bool bad = !std::isfinite(s.y[k]) || !std::isfinite(X[k]) || (d_.logY && s.y[k] <= 0);
            if (bad) { flush(); continue; }
            // skip runs far outside the view (keep one point on each side so the line enters / leaves the plot)
            const bool in = X[k] >= xl && X[k] <= xr;
            const bool prevIn = k > 0 && std::isfinite(X[k - 1]) && X[k - 1] >= xl && X[k - 1] <= xr;
            const bool nextIn = k + 1 < n && std::isfinite(X[k + 1]) && X[k + 1] >= xl && X[k + 1] <= xr;
            const bool crosses = k + 1 < n && std::isfinite(X[k + 1]) && ((X[k] < xl && X[k + 1] > xr) || (X[k] > xr && X[k + 1] < xl));
            if (!in && !prevIn && !nextIn && !crosses) { flush(); continue; }
            const QPointF q(X[k], PY(s.y[k]));
            if (!pts.empty() && std::fabs(q.x() - pts.back().x()) < 0.05 && std::fabs(q.y() - pts.back().y()) < 0.05) continue;
            pts.push_back(q);
        }
        flush();
    }
}

// ---- GUI thread: prepare layers and meshes; render thread: hand them to the scene graph ------------------------------------
void PlotItem::updatePolish() {
    if (width() <= 0 || height() <= 0) return;
    ProfScope prof_("plot.prepare");
    clip_ = area().adjusted(-1, -1, 1, 1);
    sw_ = window() && window()->rendererInterface()->graphicsApi() == QSGRendererInterface::Software;
    QByteArray bk = backKey();
    if (bk != back_key_ || (sw_ && mesh_dirty_)) {
        back_key_ = bk;
        back_ = layerImage();
        QPainter p(&back_);
        paintBack(&p);
        if (sw_) { paintSeriesSw(&p); mesh_dirty_ = false; }
        back_new_ = true;
    }
    QByteArray fk = frontKey();
    if (fk != front_key_) {
        front_key_ = fk;
        front_ = layerImage();
        QPainter p(&front_);
        paintFront(&p);
        front_new_ = true;
    }
    if (mesh_dirty_) {
        buildMesh();
        mesh_dirty_ = false;
        mesh_new_ = true;
    }
    if (d_.heat.valid && !d_.heat.img.isNull()) {
        heat_rect_ = QRectF(QPointF(tx(d_.heat.x0), ty(d_.heat.y1)), QPointF(tx(d_.heat.x1), ty(d_.heat.y0))).normalized();
        if (d_.heat.img.cacheKey() != heat_key_) { heat_key_ = d_.heat.img.cacheKey(); heat_new_ = true; }
    } else if (heat_key_ != 0) {
        heat_key_ = 0;
        heat_new_ = true;
    }
}

// software scene graph: no custom geometry / materials there, so lines, bars and the heatmap go into the back layer
void PlotItem::paintSeriesSw(QPainter* p) {
    const QRectF A = area();
    auto fx = [&](double v) { return d_.logX ? std::log10(std::max(v, 1e-30)) : v; };
    auto fy = [&](double v) { return d_.logY ? std::log10(std::max(v, 1e-30)) : v; };
    p->save();
    p->setClipRect(A.adjusted(-1, -1, 1, 1));
    if (d_.heat.valid && !d_.heat.img.isNull())
        p->drawImage(QRectF(QPointF(tx(d_.heat.x0), ty(d_.heat.y1)), QPointF(tx(d_.heat.x1), ty(d_.heat.y0))).normalized(), d_.heat.img);
    const int cols = std::max(1, (int)A.width());
    for (const auto& s : d_.series) {
        const size_t n = std::min(s.x.size(), s.y.size());
        if (s.kind == PlotSeries::Bars) {
            for (size_t k = 0; k < n; ++k)
                if (std::isfinite(s.y[k]))
                    p->fillRect(QRectF(QPointF(tx(s.x[k] - s.barWidth / 2), ty(fy(s.y[k]))), QPointF(tx(s.x[k] + s.barWidth / 2), ty(d_.logY ? vy0_ : 0))).normalized(), s.color);
            continue;
        }
        if (s.kind != PlotSeries::Line) continue;
        QPen pen(s.color, std::max(1.f, s.width));
        pen.setCosmetic(true);
        p->setPen(pen);
        p->setRenderHint(QPainter::Antialiasing, n <= (size_t)2 * cols);
        std::vector<QPointF> pts;
        if (n > (size_t)2 * cols) {   // min / max per pixel column
            std::vector<double> lo(cols, 1e300), hi(cols, -1e300);
            for (size_t k = 0; k < n; ++k) {
                if (!std::isfinite(s.y[k]) || !std::isfinite(s.x[k])) continue;
                const int c = (int)(tx(fx(s.x[k])) - A.left());
                if (c < 0 || c >= cols) continue;
                const double Y = ty(fy(s.y[k]));
                lo[c] = std::min(lo[c], Y); hi[c] = std::max(hi[c], Y);
            }
            for (int c = 0; c < cols; ++c) if (lo[c] <= hi[c]) { pts.emplace_back(A.left() + c + 0.5, lo[c]); pts.emplace_back(A.left() + c + 0.5, hi[c]); }
            if (pts.size() >= 2) p->drawPolyline(pts.data(), (int)pts.size());
            continue;
        }
        for (size_t k = 0; k <= n; ++k) {
            const bool ok = k < n && std::isfinite(s.y[k]) && std::isfinite(s.x[k]) && !(d_.logY && s.y[k] <= 0);
            if (ok) { pts.emplace_back(tx(fx(s.x[k])), ty(fy(s.y[k]))); continue; }
            if (pts.size() >= 2) p->drawPolyline(pts.data(), (int)pts.size());
            pts.clear();
        }
    }
    p->restore();
}

// node tree: root -> [back image, clip -> [heat image, mesh], front image]; software renderer: root -> [back, front]
QSGNode* PlotItem::updatePaintNode(QSGNode* old, UpdatePaintNodeData*) {
    if (width() <= 0 || height() <= 0 || back_.isNull()) { delete old; back_new_ = front_new_ = mesh_new_ = heat_new_ = true; return nullptr; }
    ProfScope prof_("plot.sync");
    if (sw_) {
        QSGNode* root = old;
        QSGImageNode *back, *front;
        if (!root) {
            root = new QSGNode;
            back = window()->createImageNode();
            back->setOwnsTexture(true);
            front = window()->createImageNode();
            front->setOwnsTexture(true);
            root->appendChildNode(back);
            root->appendChildNode(front);
            back_new_ = front_new_ = true;
        } else {
            back = static_cast<QSGImageNode*>(root->childAtIndex(0));
            front = static_cast<QSGImageNode*>(root->childAtIndex(1));
        }
        const QRectF full(0, 0, width(), height());
        if (back_new_) { back->setTexture(window()->createTextureFromImage(back_, QQuickWindow::TextureHasAlphaChannel)); back_new_ = false; }
        if (front_new_) { front->setTexture(window()->createTextureFromImage(front_, QQuickWindow::TextureHasAlphaChannel)); front_new_ = false; }
        back->setRect(full);
        front->setRect(full);
        return root;
    }
    QSGNode* root = old;
    QSGImageNode *back, *front, *heat;
    QSGClipNode* clip;
    QSGGeometryNode* mesh;
    if (!root) {
        root = new QSGNode;
        back = window()->createImageNode();
        back->setOwnsTexture(true);
        clip = new QSGClipNode;
        clip->setIsRectangular(true);
        auto* cg = new QSGGeometry(QSGGeometry::defaultAttributes_Point2D(), 4);
        clip->setGeometry(cg);
        clip->setFlag(QSGNode::OwnsGeometry);
        heat = window()->createImageNode();
        heat->setOwnsTexture(true);
        heat->setFiltering(QSGTexture::Linear);
        mesh = new QSGGeometryNode;
        auto* g = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0, 0, QSGGeometry::UnsignedIntType);
        g->setDrawingMode(QSGGeometry::DrawTriangles);
        mesh->setGeometry(g);
        mesh->setFlag(QSGNode::OwnsGeometry);
        mesh->setMaterial(new QSGVertexColorMaterial);
        mesh->setFlag(QSGNode::OwnsMaterial);
        front = window()->createImageNode();
        front->setOwnsTexture(true);
        root->appendChildNode(back);
        root->appendChildNode(clip);
        clip->appendChildNode(heat);
        clip->appendChildNode(mesh);
        root->appendChildNode(front);
        back_new_ = front_new_ = mesh_new_ = heat_new_ = true;
    } else {
        back = static_cast<QSGImageNode*>(root->childAtIndex(0));
        clip = static_cast<QSGClipNode*>(root->childAtIndex(1));
        front = static_cast<QSGImageNode*>(root->childAtIndex(2));
        heat = static_cast<QSGImageNode*>(clip->childAtIndex(0));
        mesh = static_cast<QSGGeometryNode*>(clip->childAtIndex(1));
    }
    const QRectF full(0, 0, width(), height());
    if (back_new_) { back->setTexture(window()->createTextureFromImage(back_, QQuickWindow::TextureHasAlphaChannel)); back_new_ = false; }
    back->setRect(full);
    if (front_new_) { front->setTexture(window()->createTextureFromImage(front_, QQuickWindow::TextureHasAlphaChannel)); front_new_ = false; }
    front->setRect(full);
    if (clip->clipRect() != clip_) {
        clip->setClipRect(clip_);
        QSGGeometry::updateRectGeometry(clip->geometry(), clip_);
        clip->markDirty(QSGNode::DirtyGeometry);
    }
    if (heat_new_) {
        if (heat_key_ != 0) heat->setTexture(window()->createTextureFromImage(d_.heat.img));
        heat_new_ = false;
    }
    heat->setRect(heat_key_ != 0 && heat->texture() ? heat_rect_ : QRectF());
    if (mesh_new_) {
        QSGGeometry* g = mesh->geometry();
        g->allocate((int)mesh_.v.size(), (int)mesh_.i.size());
        if (!mesh_.v.empty()) std::memcpy(g->vertexDataAsColoredPoint2D(), mesh_.v.data(), mesh_.v.size() * sizeof(V));
        if (!mesh_.i.empty()) std::memcpy(g->indexDataAsUInt(), mesh_.i.data(), mesh_.i.size() * sizeof(quint32));
        mesh->markDirty(QSGNode::DirtyGeometry);
        mesh_new_ = false;
    }
    return root;
}

// ---- interaction ------------------------------------------------------------------------------------------------------------
void PlotItem::mousePressEvent(QMouseEvent* e) {
    pressed_ = true; moved_ = false;
    press_ = last_ = e->position();
    drag_mark_ = -1;
    auto fx = [&](double v) { return d_.logX ? std::log10(std::max(v, 1e-30)) : v; };
    for (size_t k = 0; k < d_.marks.size(); ++k)
        if (d_.marks[k].dragId >= 0 && !d_.marks[k].horizontal && std::abs(tx(fx(d_.marks[k].v)) - e->position().x()) < 6) { drag_mark_ = (int)k; break; }
    e->accept();
}

void PlotItem::mouseMoveEvent(QMouseEvent* e) {
    if (!pressed_) return;
    QPointF d = e->position() - last_;
    last_ = e->position();
    hover_ = e->position();
    if ((e->position() - press_).manhattanLength() > 3) moved_ = true;
    if (!moved_) return;
    bool geom = false;
    if (drag_mark_ >= 0) {
        double X = ix(e->position().x());
        if (d_.logX) X = std::pow(10, X);
        d_.marks[drag_mark_].v = X;
        emit markDragged(d_.marks[drag_mark_].dragId, X);
    } else {
        QRectF A = area();
        double dx = -d.x() / A.width() * (vx1_ - vx0_), dy = d.y() / A.height() * (vy1_ - vy0_);
        vx0_ += dx; vx1_ += dx; user_x_ = true;
        if (!d_.fixY || e->modifiers() & Qt::ShiftModifier) { vy0_ += dy; vy1_ += dy; user_y_ = true; }
        pushView(false);
        geom = true;
    }
    pushHover();
    redraw(geom);
}

void PlotItem::mouseReleaseEvent(QMouseEvent* e) {
    if (pressed_ && !moved_ && area().contains(e->position())) {
        double X = ix(e->position().x()), Y = iy(e->position().y());
        if (d_.logX) X = std::pow(10, X);
        if (d_.logY) Y = std::pow(10, Y);
        emit clicked(X, Y);
    }
    pressed_ = false;
    drag_mark_ = -1;
}

void PlotItem::mouseDoubleClickEvent(QMouseEvent*) { resetView(); }

void PlotItem::wheelEvent(QWheelEvent* e) {
    double k = std::pow(0.85, e->angleDelta().y() / 120.0);
    QPointF pos = e->position();
    if (e->modifiers() & Qt::ShiftModifier) {
        double c = iy(pos.y());
        vy0_ = c + (vy0_ - c) * k; vy1_ = c + (vy1_ - c) * k; user_y_ = true;
    } else {
        double c = ix(pos.x());
        vx0_ = c + (vx0_ - c) * k; vx1_ = c + (vx1_ - c) * k; user_x_ = true;
    }
    pushView(false);
    redraw(true);
    e->accept();
}

void PlotItem::hoverMoveEvent(QHoverEvent* e) { hover_ = e->position(); pushHover(); redraw(false); }
void PlotItem::hoverLeaveEvent(QHoverEvent*) { hover_ = QPointF(-1, -1); pushHover(); redraw(false); }

}  // namespace hz

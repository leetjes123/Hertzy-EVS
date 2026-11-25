#include "liveview.hpp"
#include <QDateTime>
#include <QQuickWindow>
#include <QSGImageNode>
#include <algorithm>
#include "prof.hpp"

namespace evs {

LiveView::LiveView(QQuickItem* parent) : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
    connect(&timer_, &QTimer::timeout, this, &LiveView::poll);
    timer_.setTimerType(Qt::PreciseTimer);
    timer_.start(8);
    fps_t0_ = QDateTime::currentMSecsSinceEpoch();
}

void LiveView::setSource(QObject* s) {
    if (s == src_) return;
    src_ = s;
    seq_ = ov_seq_ = 0;
    emit sourceChanged();
}

void LiveView::poll() {
    auto* c = dynamic_cast<FrameSource*>(src_);
    if (!c || !isVisible()) return;
    QImage ov;
    if (c->takeOverlay(ov, ov_seq_)) { ov_ = ov; ov_fresh_ = true; update(); }
    QImage im;
    if (!c->takeFrame(im, seq_)) return;
    if (im.width() != iw_ || im.height() != ih_) {
        iw_ = im.width();
        ih_ = im.height();
        cx_ = iw_ / 2.0;
        cy_ = ih_ / 2.0;
        emit imageChanged();
        emit viewChanged();
    }
    img_ = im;
    fresh_ = true;
    ++frames_;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - fps_t0_ >= 500) {
        fps_ = frames_ * 1000.0 / (now - fps_t0_);
        frames_ = 0;
        fps_t0_ = now;
        emit fpsChanged();
    }
    update();
}

double LiveView::pxScale() const {
    const double fitS = std::min(width() / std::max(1, iw_), height() / std::max(1, ih_));
    return std::max(1e-6, fitS * zoom_);
}

QPointF LiveView::toImage(double x, double y) const {
    const double s = pxScale();
    return QPointF((x - width() / 2) / s + cx_, (y - height() / 2) / s + cy_);
}

QPointF LiveView::toItem(double x, double y) const {
    const double s = pxScale();
    return QPointF((x - cx_) * s + width() / 2, (y - cy_) * s + height() / 2);
}

void LiveView::setZoom(double z) {
    z = std::clamp(z, 0.25, 64.0);
    if (z == zoom_) return;
    zoom_ = z;
    emit viewChanged();
    update();
}
void LiveView::setCx(double v) { if (v != cx_) { cx_ = v; emit viewChanged(); update(); } }
void LiveView::setCy(double v) { if (v != cy_) { cy_ = v; emit viewChanged(); update(); } }

void LiveView::zoomAt(double x, double y, double factor) {
    const QPointF before = toImage(x, y);
    zoom_ = std::clamp(zoom_ * factor, 0.25, 64.0);
    const double s = pxScale();
    cx_ = before.x() - (x - width() / 2) / s;
    cy_ = before.y() - (y - height() / 2) / s;
    emit viewChanged();
    update();
}

void LiveView::panBy(double dx, double dy) {
    const double s = pxScale();
    cx_ -= dx / s;
    cy_ -= dy / s;
    emit viewChanged();
    update();
}

void LiveView::fit() {
    zoom_ = 1;
    cx_ = iw_ / 2.0;
    cy_ = ih_ / 2.0;
    emit viewChanged();
    update();
}

void LiveView::zoomTo(double x0, double y0, double x1, double y1) {
    const double w = std::max(4.0, std::fabs(x1 - x0)), h = std::max(4.0, std::fabs(y1 - y0));
    const double fitS = std::min(width() / std::max(1, iw_), height() / std::max(1, ih_));
    zoom_ = std::clamp(std::min(width() / w, height() / h) / fitS * 0.92, 0.25, 64.0);
    cx_ = (x0 + x1) / 2;
    cy_ = (y0 + y1) / 2;
    emit viewChanged();
    update();
}

void LiveView::geometryChange(const QRectF& n, const QRectF& o) {
    QQuickItem::geometryChange(n, o);
    emit viewChanged();
    update();
}

// root node with the frame and the optional overlay as image nodes
QSGNode* LiveView::updatePaintNode(QSGNode* old, UpdatePaintNodeData*) {
    hz::ProfScope prof_("view.sync");
    if (img_.isNull()) { delete old; return nullptr; }
    QSGNode* root = old;
    if (!root) {
        root = new QSGNode;
        fresh_ = true;
        ov_fresh_ = true;
    }
    auto* node = static_cast<QSGImageNode*>(root->firstChild());
    if (!node) {
        node = window()->createImageNode();
        node->setOwnsTexture(true);
        node->setFiltering(QSGTexture::Nearest);   // event pixels stay sharp when zoomed in
        root->appendChildNode(node);
        fresh_ = true;
    }
    if (fresh_) {
        node->setTexture(window()->createTextureFromImage(img_));
        fresh_ = false;
    }
    auto* onode = static_cast<QSGImageNode*>(node->nextSibling());
    if (ov_fresh_) {
        if (onode) { root->removeChildNode(onode); delete onode; onode = nullptr; }
        if (!ov_.isNull() && ov_.size() == img_.size()) {
            onode = window()->createImageNode();
            onode->setOwnsTexture(true);
            onode->setFiltering(QSGTexture::Nearest);
            onode->setTexture(window()->createTextureFromImage(ov_, QQuickWindow::TextureHasAlphaChannel));
            root->appendChildNode(onode);
        }
        ov_fresh_ = false;
    }
    const QPointF tl = toItem(0, 0), br = toItem(iw_, ih_);
    // clip to the item: map the visible item area back to a texture sub-rectangle
    const QRectF full(tl, br), vis = full.intersected(QRectF(0, 0, width(), height()));
    const double s = pxScale();
    const QRectF src = vis.isEmpty() ? QRectF() : QRectF((vis.left() - tl.x()) / s, (vis.top() - tl.y()) / s, vis.width() / s, vis.height() / s);
    for (QSGImageNode* n : {node, onode}) {
        if (!n) continue;
        n->setRect(vis.isEmpty() ? QRectF() : vis);
        if (!vis.isEmpty()) n->setSourceRect(src);
    }
    return root;
}

}  // namespace evs

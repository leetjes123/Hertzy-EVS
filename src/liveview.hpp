// Live event image as a scene-graph texture (no QPainter in the render path), with zoom / pan and the mapping
// between item and sensor coordinates for the QML overlays (probes, ROI, measuring line).
#pragma once
#include <QImage>
#include <QPointF>
#include <QQuickItem>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

namespace evs {

// what an EventView shows: the controller hands out its newest picture (sensor size); false when nothing new since seq
class FrameSource {
public:
    virtual ~FrameSource() = default;
    virtual bool takeFrame(QImage& out, uint64_t& seq) = 0;
    // optional transparent layer on top (painted masks, reference picture), same size as the frame; a null image hides it
    virtual bool takeOverlay(QImage& out, uint64_t& seq) { (void)out; (void)seq; return false; }
};

class LiveView : public QQuickItem {
    Q_OBJECT
    QML_NAMED_ELEMENT(EventView)
    Q_PROPERTY(QObject* source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(double zoom READ zoom WRITE setZoom NOTIFY viewChanged)     // 1 = whole sensor fits
    Q_PROPERTY(double cx READ cx WRITE setCx NOTIFY viewChanged)           // sensor point in the item centre
    Q_PROPERTY(double cy READ cy WRITE setCy NOTIFY viewChanged)
    Q_PROPERTY(double scale READ pxScale NOTIFY viewChanged)               // item px per sensor px
    Q_PROPERTY(int imageWidth READ imageWidth NOTIFY imageChanged)
    Q_PROPERTY(int imageHeight READ imageHeight NOTIFY imageChanged)
    Q_PROPERTY(double fps READ fps NOTIFY fpsChanged)
public:
    explicit LiveView(QQuickItem* parent = nullptr);
    QObject* source() const { return src_; }
    void setSource(QObject* s);
    double zoom() const { return zoom_; }
    void setZoom(double z);
    double cx() const { return cx_; }
    void setCx(double v);
    double cy() const { return cy_; }
    void setCy(double v);
    double pxScale() const;
    int imageWidth() const { return iw_; }
    int imageHeight() const { return ih_; }
    double fps() const { return fps_; }

    Q_INVOKABLE QPointF toImage(double x, double y) const;
    Q_INVOKABLE QPointF toItem(double x, double y) const;
    Q_INVOKABLE void zoomAt(double x, double y, double factor);   // keeps the sensor point under (x, y)
    Q_INVOKABLE void panBy(double dx, double dy);                 // item pixels
    Q_INVOKABLE void fit();
    Q_INVOKABLE void zoomTo(double x0, double y0, double x1, double y1);   // sensor rectangle

signals:
    void sourceChanged();
    void viewChanged();
    void imageChanged();
    void fpsChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* old, UpdatePaintNodeData*) override;
    void geometryChange(const QRectF& n, const QRectF& o) override;

private:
    void poll();
    QObject* src_ = nullptr;
    QTimer timer_;
    QImage img_, ov_;
    uint64_t seq_ = 0, ov_seq_ = 0;
    bool fresh_ = false, ov_fresh_ = false;
    int iw_ = 1280, ih_ = 720;
    double zoom_ = 1, cx_ = 640, cy_ = 360;
    int frames_ = 0;
    qint64 fps_t0_ = 0;
    double fps_ = 0;
};

}  // namespace evs

// Lightweight 2-D plot for QML: lines (min/max decimated), scatter, bars, heatmaps, marker lines that can be dragged,
// text labels, legend, hover readout, wheel zoom / drag pan / double-click reset. Data comes from the Controller
// (fillPlot) by plot "kind"; the item refreshes on Controller::plotsChanged (skipped while the plot is hidden).
//
// Rendering is built for live data at 30-60 Hz: lines and bars are anti-aliased triangle meshes drawn by the GPU
// (scene graph), the heatmap is a texture, and the static parts (grid, ticks, labels / legend, marks, texts, hover) are
// painted with QPainter into two cached layers that are only repainted when their content changes.
#pragma once
#include <QByteArray>
#include <QColor>
#include <QImage>
#include <QQuickItem>
#include <QPainter>
#include <QSGGeometry>
#include <QtQml/qqmlregistration.h>
#include <cmath>
#include <vector>

namespace hz {
struct PlotData;

// what a Plot draws from: fillPlot(kind) on every plotsChanged
class PlotSource : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual void fillPlot(const QString& kind, PlotData& d) = 0;
signals:
    void plotsChanged();
};

struct PlotSeries {
    enum Kind { Line, Scatter, Bars };
    Kind kind = Line;
    QString name;
    QColor color;
    std::vector<double> x, y;
    float width = 1.5f;
    int marker = 0;          // 0 circle, 1 square, 2 diamond
    float size = 6;
    double barWidth = 0.7;
    bool legend = true;
    bool hollow = false;
    int style = 0;           // line style: 0 solid, 1 dashed, 2 dotted, 3 dash-dot, 4 dash-dot-dot
};
struct PlotMark { double v = 0; QColor color; float width = 1; bool dashed = false; QString label; int dragId = -1; bool horizontal = false; };
struct PlotText { double x = 0, y = 0; QString text; QColor color; };
struct PlotHeat { QImage img; double x0 = 0, x1 = 1, y0 = 0, y1 = 1; bool valid = false; };
struct PlotData {
    QString xlabel, ylabel, empty = "No data yet";
    bool logY = false, logX = false;
    bool fixX = false, fixY = false;
    double x0 = 0, x1 = 1, y0 = 0, y1 = 1;
    bool yFromZero = false;
    std::vector<PlotSeries> series;
    std::vector<PlotMark> marks;
    std::vector<PlotText> texts;
    PlotHeat heat;
    bool legend = true;
    QString xunit, yunit;    // for the hover readout
};

QRgb viridis(float t);

class PlotItem : public QQuickItem {
    Q_OBJECT
    QML_NAMED_ELEMENT(Plot)
    Q_PROPERTY(QObject* source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(QString kind READ kind WRITE setKind NOTIFY kindChanged)
    Q_PROPERTY(double cursorX READ cursorX WRITE setCursorX NOTIFY cursorXChanged)
    Q_PROPERTY(bool hasData READ hasData NOTIFY dataChanged)
    // plots with the same non-empty syncGroup share their x view (zoom, pan, reset) and hover cursor; syncY also the y view
    Q_PROPERTY(QString syncGroup READ syncGroup WRITE setSyncGroup NOTIFY syncChanged)
    Q_PROPERTY(bool syncY READ syncY WRITE setSyncY NOTIFY syncChanged)
public:
    explicit PlotItem(QQuickItem* parent = nullptr);
    ~PlotItem() override;
    QString syncGroup() const { return group_; }
    void setSyncGroup(const QString& g);
    bool syncY() const { return sync_y_; }
    void setSyncY(bool v);
    QObject* source() const;
    void setSource(QObject* s);
    QString kind() const { return kind_; }
    void setKind(const QString& k);
    double cursorX() const { return cursor_; }
    void setCursorX(double v);
    bool hasData() const { return !d_.series.empty() || d_.heat.valid; }
    Q_INVOKABLE void resetView();
    Q_INVOKABLE void refresh();

signals:
    void sourceChanged();
    void kindChanged();
    void cursorXChanged();
    void dataChanged();
    void syncChanged();
    void clicked(double x, double y);
    void markDragged(int id, double value);

protected:
    QSGNode* updatePaintNode(QSGNode* old, UpdatePaintNodeData*) override;
    void updatePolish() override;
    void geometryChange(const QRectF& n, const QRectF& o) override;
    void itemChange(ItemChange c, const ItemChangeData& v) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    void hoverMoveEvent(QHoverEvent* e) override;
    void hoverLeaveEvent(QHoverEvent* e) override;

private:
    QRectF area() const;
    bool showYLabel() const { return !d_.ylabel.isEmpty() && height() >= 140; }
    bool showXLabel() const { return !d_.xlabel.isEmpty() && height() >= 110; }
    double tx(double x) const;   // data -> pixel
    double ty(double y) const;
    double ix(double px) const;  // pixel -> data
    double iy(double py) const;
    void autoRange();
    std::vector<PlotItem*> peers() const;
    void pushView(bool reset);     // copy this view to the peers
    void adoptView();              // take a zoomed peer's view (new or refreshed plot)
    void pushHover();
    void redraw(bool geometry);    // schedule a polish + sync; geometry = rebuild the line / bar meshes
    QByteArray backKey() const;
    QByteArray frontKey() const;
    void paintBack(QPainter* p);   // grid, ticks, axis labels
    void paintFront(QPainter* p);  // scatter, marks, cursors, texts, empty state, legend, hover readout
    void buildMesh();
    void paintSeriesSw(QPainter* p);   // software scene graph (offscreen, VNC, no GL): lines / bars / heat with QPainter
    QImage layerImage() const;

    PlotSource* src_ = nullptr;
    QString kind_;
    PlotData d_;
    double cursor_ = NAN;
    // view (in transformed units: log10 for log axes)
    double vx0_ = 0, vx1_ = 1, vy0_ = 0, vy1_ = 1;
    bool user_x_ = false, user_y_ = false;
    // interaction
    bool pressed_ = false, moved_ = false;
    int drag_mark_ = -1;
    QPointF press_, last_;
    QPointF hover_{-1, -1};
    QString group_;
    bool sync_y_ = false;
    double peer_hover_ = NAN;   // a peer's hover x, in transformed units
    bool stale_ = false;        // data changed while hidden: refresh when shown

    // render state, prepared on the GUI thread (updatePolish), handed to the scene graph in updatePaintNode
    struct Mesh {
        std::vector<QSGGeometry::ColoredPoint2D> v;
        std::vector<quint32> i;
    };
    Mesh mesh_;
    bool mesh_dirty_ = true, mesh_new_ = true;
    QImage back_, front_;
    QByteArray back_key_, front_key_;
    bool back_new_ = false, front_new_ = false;
    qint64 heat_key_ = 0;
    bool heat_new_ = false;
    QRectF clip_, heat_rect_;
    bool sw_ = false;
};

}  // namespace hz

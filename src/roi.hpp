// Regions of interest on the sensor: line (with side bands), rectangle, polygon and painted mask.
// A ROI rasterises to a list of pixels with a sign. Line ROIs are "sided": pixels on the +normal side count -1 and on the
// -normal side +1, so the ON-OFF events of an edge that moves sideways add up instead of cancelling (a dark bar moving
// towards +normal makes OFF on its +side edge and ON on its -side edge). Area ROIs count every pixel +1.
#pragma once
#include <QColor>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QVariantMap>
#include <cstdint>
#include <vector>

namespace evs {

enum class RoiType { Line, Rect, Polygon, Mask };

// what is counted per time bin
enum class SignalKind {
    Auto,       // line: sided ON-OFF, area: ON-OFF
    OnOff,      // ON - OFF (sided for lines)
    On,         // ON count
    Off,        // OFF count
    Count       // ON + OFF (an oscillating edge shows up at 2f as well)
};
QString signal_name(SignalKind k);
SignalKind signal_from(const QString& s);

struct Roi {
    int id = 0;
    QString name;
    QColor color;
    RoiType type = RoiType::Line;
    SignalKind signal = SignalKind::Auto;
    std::vector<QPointF> pts;           // line: 2 end points; rect: 2 corners; polygon: >= 3 vertices
    double half = 6;                     // line: half-width of each side band (px)
    std::vector<uint8_t> mask;           // mask: W*H, 1 = inside (empty for other types)
    int maskW = 0, maskH = 0;
    QString source;                      // "drawn", "lines", "blobs", ...

    bool sided() const { return type == RoiType::Line && (signal == SignalKind::Auto || signal == SignalKind::OnOff); }
    double length() const;
    QRectF bounds() const;
};

struct RoiPixels {
    std::vector<uint32_t> idx;           // y * W + x
    std::vector<int8_t> sign;            // +1 / -1
};
RoiPixels rasterize(const Roi& r, int W, int H);
// pixel -> ROI look-up (first ROI wins where they overlap): value = ROI index + 1, sign separately
void build_lut(const std::vector<Roi>& rois, int W, int H, std::vector<uint16_t>& lut, std::vector<int8_t>& sign);

QJsonObject roi_to_json(const Roi& r);
Roi roi_from_json(const QJsonObject& o, int W, int H);
QVariantMap roi_to_variant(const Roi& r);    // for QML (no mask pixels)

// run-length text of a binary mask ("start:len,start:len" over the flattened image)
QString mask_rle(const std::vector<uint8_t>& m);
std::vector<uint8_t> mask_from_rle(const QString& s, size_t n);

QColor roi_color(int i);

// list of ROIs with ids, names and colours; shared by the live and the analysis page
class RoiSet {
public:
    std::vector<Roi> rois;
    int selected = -1;                   // id
    int nextId = 1;
    int add(Roi r);                      // assigns id / name / colour when missing; returns the id
    Roi* find(int id);
    const Roi* find(int id) const;
    int indexOf(int id) const;
    bool remove(int id);
    void clear() { rois.clear(); selected = -1; nextId = 1; }
    QJsonArray toJson() const;
    void fromJson(const QJsonArray& a, int W, int H);
};

}  // namespace evs

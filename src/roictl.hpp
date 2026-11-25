// ROI editing shared by the live page (EvsController) and the analysis page (AnalyseController): QML draws and edits
// the ROIs through the same invokables (RoiLayer.qml), painted masks are shown in the view's overlay layer.
#pragma once
#include <QImage>
#include <QVariantList>
#include <mutex>
#include "liveview.hpp"
#include "plotitem.hpp"
#include "roi.hpp"

namespace evs {

class RoiController : public hz::PlotSource, public FrameSource {
    Q_OBJECT
    Q_PROPERTY(QVariantList rois READ roiList NOTIFY roisChanged)
    Q_PROPERTY(int selRoi READ selRoi NOTIFY roisChanged)
public:
    using hz::PlotSource::PlotSource;
    QVariantList roiList() const;
    int selRoi() const { return rs_.selected; }

    // type: line | rect | polygon | mask; pts: [{x, y}, ...] in sensor pixels
    Q_INVOKABLE int addRoi(const QString& type, const QVariantList& pts);
    Q_INVOKABLE void moveRoi(int id, const QVariantList& pts);
    Q_INVOKABLE void translateRoi(int id, double dx, double dy);
    Q_INVOKABLE void setRoiProp(int id, const QString& key, const QVariant& v);   // name, half, signal, color
    Q_INVOKABLE void removeRoi(int id);
    Q_INVOKABLE void clearRois();
    Q_INVOKABLE void selectRoi(int id);
    Q_INVOKABLE int duplicateRoi(int id);
    // brush: paints a disc into mask ROI `id` (id <= 0 or not a mask: a new mask ROI); returns the mask's id
    Q_INVOKABLE int paintMask(int id, double x, double y, double r, bool erase);
    Q_INVOKABLE void endStroke();   // brush released: heavy updates (engine LUT, analysis) happen here

    bool takeOverlay(QImage& out, uint64_t& seq) override;
    const RoiSet& roiSet() const { return rs_; }

signals:
    void roisChanged();

protected:
    virtual int sensorW() const { return 1280; }
    virtual int sensorH() const { return 720; }
    // called after every edit; geometry = pixels changed (not just name / colour / selection)
    virtual void onRoisEdited(bool geometry) { (void)geometry; }
    // extra layer under the masks (live: reference picture)
    virtual void decorateOverlay(QImage& img) { (void)img; }
    void edited(bool geometry);
    void rebuildOverlay();
    void setRois(std::vector<Roi> rois);

    RoiSet rs_;
    bool stroking_ = false;
    bool havePaint_ = false;      // last brush point of the current stroke (strokes are filled in between events)
    QPointF lastPaint_;

private:
    std::mutex ovmx_;
    QImage ov_;
    uint64_t ovSeq_ = 1;
};

}  // namespace evs

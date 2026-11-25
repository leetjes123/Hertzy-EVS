// Playback of a decoded clip on the analysis page (QML: `player`): slow motion without motion blur, strobe, trails,
// plots that follow the playhead, and MP4 export.
//
// An event camera has no frames: every picture is made from the events of a short exposure ending at the playhead,
// so the exposure (e.g. 50 us = a 20 000 fps camera) is independent of the playback speed (e.g. 1/1000 x).
//   events  ON / OFF of the exposure, newer = brighter
//   trail   per pixel the time of its last event, coloured by age over the trail length: a moving object leaves a
//           timed path (hover / speed tool read the time stamps)
//   heat    event count per pixel in the exposure (log)
// Strobe: every picture steps one period (1 / f) plus a small drift, so periodic motion (fan, gear, shaker) stands
// still or turns slowly however fast it is.
// Frames are rendered on the player thread; the GUI only picks up the newest picture (EventView) and the position.
#pragma once
#include <QImage>
#include <QTimer>
#include <QVariantMap>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include "clip.hpp"
#include "liveview.hpp"
#include "measure.hpp"
#include "plotitem.hpp"
#include "roi.hpp"

namespace evs {

class RoiController;

struct PlayParams {
    QString mode = "events";        // events | trail | heat
    double speed = 0.01;            // clip seconds per wall-clock second (0.01 = 1/100 x)
    double exposureUs = 200;        // events per picture: (t - exposure, t]
    double trailMs = 5;             // trail mode: age that fades to nothing
    bool strobe = false;
    double strobeHz = 50;           // one picture per period
    double strobeDrift = 1;         // % of a period per picture (0 = frozen)
    double fps = 60;                // pictures per wall-clock second (strobe: 30)
    bool bold = true;               // events / heat: every event as 2 x 2 px (visible when the picture is scaled down)
};

// makes pictures from a clip; keeps the trail's time surface between calls so playing forward is incremental
class FrameRenderer {
public:
    QImage render(const Clip& c, const PlayParams& p, double t);
    void reset() { last_ = -1; }
    // time (s) of the newest event within r px of (x, y) in the time surface (trail mode), < 0 = none
    double timeAt(const Clip& c, double x, double y, int r) const;
    void ensureSurface(const Clip& c, double t, double spanS);
private:
    std::vector<uint32_t> ts_;      // per pixel: time of the last event (us) + 1, 0 = none
    std::vector<uint8_t> tp_;       // its polarity
    std::vector<float> cnt_;
    double last_ = -1, span_ = 0;
    int W_ = 0, H_ = 0;
};

class Player : public hz::PlotSource, public FrameSource {
    Q_OBJECT
    Q_PROPERTY(double t READ t NOTIFY posChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY posChanged)
    Q_PROPERTY(bool loaded READ loaded NOTIFY clipChanged)
    Q_PROPERTY(double duration READ duration NOTIFY clipChanged)
    Q_PROPERTY(QVariantMap cfg READ cfg NOTIFY cfgChanged)
    Q_PROPERTY(QVariantMap exportState READ exportState NOTIFY exportChanged)
    Q_PROPERTY(QVariantList tones READ tones NOTIFY tonesChanged)
public:
    explicit Player(RoiController* rois, QObject* parent = nullptr);
    ~Player() override;

    double t() const { return t_.load(); }
    bool playing() const { return playing_.load(); }
    bool loaded() const { return (bool)clipGui_; }
    double duration() const { return clipGui_ ? clipGui_->duration : 0; }
    QVariantMap cfg() const { return cfg_; }
    QVariantMap exportState() const { return exp_; }
    QVariantList tones() const;

    bool takeFrame(QImage& out, uint64_t& seq) override;
    bool takeOverlay(QImage& out, uint64_t& seq) override;
    void fillPlot(const QString& kind, hz::PlotData& d) override;

    // from the analysis controller
    void setClip(std::shared_ptr<const Clip> c);
    void setRois(const std::vector<Roi>& rois, const MeasureSettings& s);
    void setWindow(double t0, double t1, bool ok);

    Q_INVOKABLE void setv(const QString& key, const QVariant& v);
    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void toggle();
    Q_INVOKABLE void seek(double s);
    Q_INVOKABLE void step(int n);                      // pictures (strobe: periods; else one exposure each)
    Q_INVOKABLE void home();                           // to the start of the loop range
    Q_INVOKABLE QVariantMap timeAt(double x, double y);   // trail time stamp near a sensor pixel: {ok, t}
    Q_INVOKABLE void exportVideo(double fps);          // the loop range with the current settings -> MP4 (or PNGs)
    Q_INVOKABLE void cancelExport();

signals:
    void posChanged();
    void clipChanged();
    void cfgChanged();
    void exportChanged();
    void tonesChanged();
    void toast(const QString& text);

private:
    void loop();
    void anaLoop();                      // playhead plots on their own thread (never slows the pictures)
    PlayParams params() const;           // GUI thread
    void range(double& a, double& b) const;
    void pollGui();
    void exportRun(std::shared_ptr<const Clip> c, PlayParams p, double a, double b, double fps, QString out, QString ffmpeg);
    static void drawOverlay(QImage& img, const std::vector<Roi>& rois, const PlayParams& p, double t, double a, double b, const QString& clipName);

    RoiController* roiCtl_;
    QVariantMap cfg_, exp_;
    std::shared_ptr<const Clip> clipGui_;
    QTimer gui_;

    // shared with the player thread (mx_)
    std::mutex mx_;
    std::condition_variable cv_;
    bool quit_ = false, dirty_ = true, plotDirty_ = true;
    std::shared_ptr<const Clip> clip_;
    PlayParams p_;
    double winA_ = 0, winB_ = 0;
    bool winOk_ = false, loopWindow_ = true;
    std::vector<Roi> rois_;
    std::vector<RoiPixels> px_;
    MeasureSettings ms_;
    double plotWin_ = 0.2;
    int stepReq_ = 0;
    std::atomic<double> t_{0};
    std::atomic<bool> playing_{false};
    std::thread th_, anTh_;
    std::condition_variable anCv_;
    double anReqT_ = -1;                 // mx_: playhead to analyse (-1 = nothing new)
    uint64_t roiGen_ = 0;                // mx_: bumped when ROIs / settings change
    // results of the player thread
    std::mutex fmx_;
    QImage frame_;
    uint64_t seq_ = 0;
    std::vector<RoiResult> res_;          // playhead analysis (fmx_)
    std::vector<std::vector<double>> sig_;
    double resT_ = -1;
    uint64_t resSeq_ = 0, resSeen_ = 0;
    FrameRenderer probe_;                 // for timeAt (GUI thread, trail surface at the playhead)
    double lastPosT_ = -1;
    bool lastPlaying_ = false;
    std::vector<RoiResult> resGui_;
    std::vector<std::vector<double>> sigGui_;
    double resTGui_ = -1;

    // export
    std::thread exTh_;
    std::atomic<bool> exCancel_{false};
    std::atomic<double> exProgress_{0};
    std::atomic<bool> exBusy_{false};
    std::mutex exmx_;
    QString exMsg_;
    bool exDone_ = false;
};

}  // namespace evs

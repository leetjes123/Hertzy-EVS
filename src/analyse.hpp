// Analysis page (QML: `ana`): one clip in memory, maps (activity, ON/OFF, dominant frequency, band energy), ROIs drawn or
// detected on the map, per-ROI spectra / tones / decay / spectrogram, the time window on the event-rate timeline, and
// named measurements saved next to the clip. Heavy work runs on a worker thread; the newest request wins.
#pragma once
#include <QTimer>
#include <QVariant>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include "clip.hpp"
#include "measure.hpp"
#include "roictl.hpp"
#include "tablemodel.hpp"

namespace evs {

class AnalyseController : public RoiController {
    Q_OBJECT
    Q_PROPERTY(QVariantMap clip READ clipInfo NOTIFY clipChanged)
    Q_PROPERTY(QVariantMap cfg READ cfg NOTIFY cfgChanged)
    Q_PROPERTY(QVariantList onsets READ onsets NOTIFY clipChanged)
    Q_PROPERTY(QVariantMap state READ state NOTIFY stateChanged)
    Q_PROPERTY(QVariantList results READ results NOTIFY resultsChanged)
    Q_PROPERTY(QVariantList measurements READ measurements NOTIFY measurementsChanged)
    Q_PROPERTY(QObject* table READ table CONSTANT)
public:
    explicit AnalyseController(QObject* parent = nullptr);
    ~AnalyseController() override;

    QVariantMap clipInfo() const { return clipInfo_; }
    QVariantMap cfg() const { return cfg_; }
    QVariantList onsets() const;
    QVariantMap state() const { return state_; }
    QVariantList results() const;
    QVariantList measurements() const { return meas_; }
    QObject* table() { return &table_; }

    bool takeFrame(QImage& out, uint64_t& seq) override;
    std::shared_ptr<const Clip> clipPtr() const { return clip_; }
    MeasureSettings measureSettings() const { return settings(); }
    void fillPlot(const QString& kind, hz::PlotData& d) override;

    Q_INVOKABLE void open(const QString& rawPath);
    Q_INVOKABLE void openMeasurement(const QString& rawPath, const QString& dir);   // open the clip, then load that measurement
    Q_INVOKABLE void close();
    Q_INVOKABLE void setCfg(const QString& key, const QVariant& v);
    Q_INVOKABLE void setWindow(double t0, double t1);      // absolute window (s)
    Q_INVOKABLE void useOnset(int k);                      // window relative to onset k (0-based)
    Q_INVOKABLE void detect(const QString& kind, bool replace);   // lines | blobs
    Q_INVOKABLE void saveMeasurement(const QString& name);
    Q_INVOKABLE void loadMeasurement(const QString& dir);
    Q_INVOKABLE void deleteMeasurement(const QString& dir);
    Q_INVOKABLE void applyRecipe(const QString& dir);       // ROIs + settings of any saved measurement
    Q_INVOKABLE QVariantList recipes() const;               // measurements of the clips in the same project
    Q_INVOKABLE QVariantMap pixelInfo(double x, double y) const;
    Q_INVOKABLE void showFolder() const;
    Q_INVOKABLE void markDrag(int id, double v);            // timeline window handles

signals:
    void clipChanged();
    void cfgChanged();
    void stateChanged();
    void resultsChanged();
    void measurementsChanged();
    void toast(const QString& text);
    void measurementSaved(const QString& rawPath);

protected:
    void onRoisEdited(bool geometry) override;

private:
    struct Job {
        uint64_t gen = 0;
        bool map = false, measure = false, sg = false, rate = false;
        std::vector<Roi> rois;
        MeasureSettings s;
        QVariantMap cfg;
        int sel = -1;
        std::shared_ptr<const Clip> clip;
        std::vector<Onset> onsets;
    };
    void post(bool map, bool measure);
    void worker();
    void runJob(const Job& j);
    void refreshMeasurements();
    void rebuildTable();
    MeasureSettings settings() const;
    void saveCfg() const;
    void setStateKey(const QString& k, const QVariant& v);

    std::shared_ptr<const Clip> clip_;
    std::vector<Onset> onsets_;
    QVariantMap clipInfo_, cfg_, state_;
    QVariantList meas_;
    hz::TableModel table_;
    QString curMeasurement_, pendingMeas_;

    // results (GUI thread)
    MeasureResult res_;
    Spectrogram2 sg_;
    int sgRoi_ = -1;
    RpmTrack rpmTrack_;
    std::vector<float> selRate_;
    int selRateRoi_ = -1;
    PixelSpectra ps_;                 // last frequency map (for pixel info)
    std::vector<float> mapOn_, mapOff_;
    QImage map_;
    uint64_t mapSeq_ = 1;
    std::mutex mapmx_;

    // worker
    std::thread th_;
    std::mutex jmx_;
    std::condition_variable jcv_;
    std::unique_ptr<Job> pending_;
    std::deque<std::function<void()>> tasks_;
    bool quit_ = false;
    std::atomic<uint64_t> gen_{0};
    std::atomic<bool> cancel_{false};
    QTimer debounce_;
    bool wantMap_ = false, wantMeasure_ = false;
    QString mapKey_;
};

}  // namespace evs

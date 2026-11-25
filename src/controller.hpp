// Live side of hertzyevs (QML: `app`): source + engine, camera settings and presets, live ROIs with rolling spectra,
// recording into the library, repeating triggered auto-measure with a log, setup aids (focus chart, reference picture,
// noise-floor finder). QML reads `st` (settings), `live` (status), `rois`, `liveTones`, `amLog`, `presets`.
#pragma once
#include <QTimer>
#include <QVariant>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include "engine.hpp"
#include "measure.hpp"
#include "roictl.hpp"
#include "source.hpp"
#include "tablemodel.hpp"
#include "webview.hpp"

namespace evs {

class EvsController : public RoiController {
    Q_OBJECT
    Q_PROPERTY(QVariantMap st READ st NOTIFY stChanged)
    Q_PROPERTY(QVariantMap live READ live NOTIFY liveChanged)
    Q_PROPERTY(QVariantList liveTones READ liveTones NOTIFY tonesChanged)
    Q_PROPERTY(QVariantList features READ features NOTIFY featuresChanged)
    Q_PROPERTY(QStringList presets READ presets NOTIFY presetsChanged)
    Q_PROPERTY(QVariantMap lastAm READ lastAm NOTIFY amChanged)
    Q_PROPERTY(int amCount READ amCount NOTIFY amChanged)
    Q_PROPERTY(QObject* amTable READ amTable CONSTANT)
    Q_PROPERTY(QString lastRecording READ lastRecording NOTIFY recordingSaved)
public:
    EvsController(const QString& source, QObject* parent = nullptr);
    ~EvsController() override;

    QVariantMap st() const { return st_; }
    QVariantMap live() const { return live_; }
    QVariantList liveTones() const;
    QVariantList features() const;
    QStringList presets() const;
    QVariantMap lastAm() const { return last_am_; }
    int amCount() const { return am_n_; }
    QObject* amTable() { return &am_table_; }
    QString lastRecording() const { return last_rec_; }

    bool takeFrame(QImage& out, uint64_t& seq) override { return engine_ && engine_->frame(out, seq); }
    void fillPlot(const QString& kind, hz::PlotData& d) override;

    void setLibraryRoot(const QString& root) { root_ = root; }
    QString libraryRoot() const { return root_; }

    Q_INVOKABLE void setv(const QString& key, const QVariant& v);
    Q_INVOKABLE void applyPreset(const QString& name);
    Q_INVOKABLE void savePreset(const QString& name);
    Q_INVOKABLE void deletePreset(const QString& name);
    Q_INVOKABLE void connectSource(const QString& kind);
    Q_INVOKABLE void toggleRun();
    Q_INVOKABLE void recordNow();
    Q_INVOKABLE void toggleArm();
    Q_INVOKABLE void cancelRecording();
    Q_INVOKABLE void simTap();
    // sensor crop (hardware ROI of the camera)
    Q_INVOKABLE void setCrop(double x0, double y0, double x1, double y1);
    Q_INVOKABLE void clearCrop();
    Q_INVOKABLE void snapRoi(int id);            // centre a line ROI on the edges that fire
    // live ROIs <-> recipes
    Q_INVOKABLE void loadRoisFrom(const QString& measurementDir);   // ROIs + analysis settings of a saved measurement
    Q_INVOKABLE void clearAmLog();
    // setup aids
    Q_INVOKABLE void captureReference();
    Q_INVOKABLE void clearReference();
    // camera page
    Q_INVOKABLE QVariantList nodeList(const QString& filter);
    Q_INVOKABLE bool setNode(const QString& name, const QString& value);
    Q_INVOKABLE void setNodeOverride(const QString& key, const QString& node);
    Q_INVOKABLE void downloadXml();
    Q_INVOKABLE void reapplyAll();
    Q_INVOKABLE void autoSensitivity();   // noise-floor finder (start / cancel); needs a still scene

signals:
    void stChanged();
    void liveChanged();
    void tonesChanged();
    void featuresChanged();
    void presetsChanged();
    void amChanged();
    void toast(const QString& text);
    void recordingSaved(const QString& path);

protected:
    void onRoisEdited(bool geometry) override;
    void decorateOverlay(QImage& img) override;

private:
    void tick();
    struct AutoSens { bool on = false; int k = 0, ticks = 0, n = 0; double sum = 0, on0 = 0, off0 = 0, gap = 0, limitKev = 20, lastKev = 0; };
    AutoSens auto_;
    void autoStep();
    void applyAutoLevel(int k);
    void finishAuto(const QString& text, bool restore);
    void applyFeature(const QString& key);
    void applyCropHw();
    void pushRois();
    void applyMeasureTrigger();
    MeasureSettings liveSettings(bool am) const;
    std::vector<double> series(int roiIndex, const ProbeTrace& tr, const MeasureSettings& s) const;
    // live ROI analysis runs on its own thread (liveLoop); the GUI thread only hands it the settings (pushLiveJob) and
    // picks up the newest signals / spectra (pullLive), so many ROIs or long windows never stall the UI
    void pushLiveJob();
    void applyWeb();
    QByteArray webJson() const;
    bool pullLive(bool& spectraNew);
    void liveLoop();
    void processAutoMeasure();
    void finishRecording(const RecStatus& rs);
    void writeSidecar(const RecStatus& rs, const QString& raw);
    void save() const;
    void load();
    QString recordingPath(const QString& tag = QString()) const;
    std::string rawHeader() const;
    double d(const char* k) const { return st_.value(k).toDouble(); }
    bool b(const char* k) const { return st_.value(k).toBool(); }

    void fillTrace(hz::PlotData& pd);
    void fillSpectrum(hz::PlotData& pd);
    void fillRate(hz::PlotData& pd);

    std::unique_ptr<EventSource> src_;
    std::unique_ptr<Engine> engine_;
    std::unique_ptr<WebView> web_;
    QVariantMap st_, live_;
    QMap<QString, QString> feat_err_;
    QTimer tick_;
    int tick_n_ = 0;
    QString root_;
    // live analysis
    std::vector<std::vector<double>> live_sig_;
    std::vector<RoiResult> live_res_;
    std::vector<float> rate_all_, rate_roi_;
    QString tones_sig_;
    int last_rec_state_ = -1;
    struct LiveJob {
        MeasureSettings s;
        double win = 0.6, won = 1, woff = 1;
        std::vector<SignalKind> kinds;
        uint64_t gen = 0;
        int rpmIdx = -1;              // ROI for the live speed (-1 = off)
        RpmSettings rpm;
    };
    std::thread lw_th_;
    std::mutex lw_mx_;
    std::condition_variable lw_cv_;
    bool lw_quit_ = false;
    LiveJob lw_job_;
    uint64_t roi_gen_ = 0;
    // worker -> GUI (lw_mx_)
    std::vector<std::vector<double>> lw_sig_;
    std::vector<RoiResult> lw_res_;
    uint64_t lw_seq_ = 0, lw_seen_ = 0, lw_gen_out_ = 0;
    bool lw_spec_new_ = false;
    RpmResult lw_rpm_;
    double lw_rpm_t_ = 0;
    bool lw_rpm_new_ = false;
    // live speed: newest result + history (stream time s, rpm; NaN = no estimate)
    RpmResult live_rpm_;
    std::deque<std::pair<double, double>> rpm_hist_;
    void fillRpm(hz::PlotData& pd);
    // auto-measure
    std::vector<int64_t> am_pending_;
    QVariantMap last_am_;
    int am_n_ = 0;
    hz::TableModel am_table_;
    std::vector<std::vector<hz::Cell>> am_rows_;
    std::vector<int> am_keys_;
    QString am_csv_;
    int rec_state_ = 0;
    QString am_clip_;
    int64_t am_clip_t_ = -1;
    // reference picture (setup aid)
    QImage ref_;
    QString last_rec_;
    QString pending_desc_;
};

}  // namespace evs

#include "analyse.hpp"
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QJsonArray>
#include <QSettings>
#include <QUrl>
#include <algorithm>
#include <cmath>
#include "detect.hpp"

namespace evs {

static const QStringList kCfgKeys = {"fs", "signal", "window", "bandLo", "bandHi", "toneLo", "toneHi", "df", "snrMin", "fmax",
                                     "commonMode", "winMode", "t0", "t1", "skip", "len", "onset", "sgWinMs", "sgHopMs",
                                     "map", "mapWin", "mapSnr", "mapMaxPix", "tool", "brushR", "compare", "lineRel", "lineMinLen",
                                     "blobMinArea", "plot", "rpmOn", "rpmMarks", "rpmLo", "rpmHi", "rpmWin"};

AnalyseController::AnalyseController(QObject* parent) : RoiController(parent) {
    const MeasureSettings s;
    cfg_ = QVariantMap{{"fs", s.fs}, {"signal", "displacement"}, {"window", s.window}, {"bandLo", s.bandLo}, {"bandHi", s.bandHi},
                       {"toneLo", s.toneLo}, {"toneHi", s.toneHi}, {"df", s.df}, {"snrMin", s.snrMin}, {"fmax", s.fmax},
                       {"commonMode", s.commonMode}, {"winMode", "abs"}, {"t0", 0.0}, {"t1", 0.0}, {"skip", 0.01}, {"len", 0.59},
                       {"onset", 0}, {"sgWinMs", s.sgWinMs}, {"sgHopMs", s.sgHopMs},
                       {"map", "activity"}, {"mapWin", "window"}, {"mapSnr", 6.0}, {"mapMaxPix", 40000}, {"tool", "select"}, {"brushR", 6.0},
                       {"compare", "all"}, {"lineRel", 0.08}, {"lineMinLen", 150.0}, {"blobMinArea", 40}, {"plot", "spectrum"},
                       {"rpmOn", s.rpmOn}, {"rpmMarks", s.rpmMarks}, {"rpmLo", s.rpmLo}, {"rpmHi", s.rpmHi}, {"rpmWin", s.rpmWin}};
    QSettings st;
    const QVariantMap m = st.value("evs/ana").toMap();
    for (auto it = m.begin(); it != m.end(); ++it) if (cfg_.contains(it.key())) cfg_[it.key()] = it.value();
    cfg_["t0"] = 0.0;
    cfg_["t1"] = 0.0;
    state_ = QVariantMap{{"loading", false}, {"busy", false}, {"progress", 0.0}};
    table_.set({"ROI", "f (Hz)", "SNR", "Amp", "Decay (ms)", "Events", "Peaks (Hz/SNR)"}, {}, {}, {56, 76, 50, 62, 78, 80, 240});
    debounce_.setSingleShot(true);
    debounce_.setInterval(120);
    connect(&debounce_, &QTimer::timeout, this, [this] {
        if (!clip_) return;
        auto j = std::make_unique<Job>();
        j->gen = ++gen_;
        j->map = wantMap_;
        j->measure = true;
        j->sg = true;
        j->rate = true;
        j->rois = rs_.rois;
        j->s = settings();
        j->cfg = cfg_;
        j->sel = rs_.selected;
        j->clip = clip_;
        j->onsets = onsets_;
        wantMap_ = wantMeasure_ = false;
        setStateKey("busy", true);
        {
            std::lock_guard<std::mutex> lk(jmx_);
            pending_ = std::move(j);
        }
        jcv_.notify_one();
    });
    th_ = std::thread([this] { worker(); });
}

AnalyseController::~AnalyseController() {
    {
        std::lock_guard<std::mutex> lk(jmx_);
        quit_ = true;
        cancel_ = true;
    }
    jcv_.notify_all();
    if (th_.joinable()) th_.join();
}

void AnalyseController::saveCfg() const {
    QVariantMap m = cfg_;
    m.remove("t0");
    m.remove("t1");
    QSettings().setValue("evs/ana", m);
}

void AnalyseController::setStateKey(const QString& k, const QVariant& v) {
    if (state_.value(k) == v) return;
    state_[k] = v;
    emit stateChanged();
}

MeasureSettings AnalyseController::settings() const {
    MeasureSettings s;
    auto dv = [&](const char* k) { return cfg_.value(k).toDouble(); };
    s.fs = std::clamp(dv("fs"), 500.0, 20000.0);
    s.signal = cfg_["signal"].toString();
    s.window = cfg_["window"].toString();
    s.bandLo = dv("bandLo");
    s.bandHi = dv("bandHi");
    s.toneLo = dv("toneLo");
    s.toneHi = dv("toneHi");
    s.df = std::max(0.02, dv("df"));
    s.snrMin = dv("snrMin");
    s.fmax = std::max(dv("fmax"), s.toneHi);
    s.commonMode = cfg_["commonMode"].toBool();
    s.winMode = cfg_["winMode"].toString();
    s.t0 = dv("t0");
    s.t1 = dv("t1");
    s.skip = dv("skip");
    s.len = dv("len");
    s.onset = cfg_["onset"].toInt();
    s.sgWinMs = dv("sgWinMs");
    s.sgHopMs = dv("sgHopMs");
    s.rpmOn = cfg_["rpmOn"].toBool();
    s.rpmMarks = std::max(1, cfg_["rpmMarks"].toInt());
    s.rpmLo = std::max(1.0, dv("rpmLo"));
    s.rpmHi = std::max(s.rpmLo + 1, dv("rpmHi"));
    s.rpmWin = std::clamp(dv("rpmWin"), 0.05, 30.0);
    return s;
}

// ---- worker -------------------------------------------------------------------------------------------------------------
void AnalyseController::worker() {
    for (;;) {
        std::unique_ptr<Job> job;
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lk(jmx_);
            jcv_.wait(lk, [&] { return quit_ || pending_ || !tasks_.empty(); });
            if (quit_) return;
            if (!tasks_.empty()) { task = std::move(tasks_.front()); tasks_.pop_front(); }
            else job.swap(pending_);
        }
        if (task) task();
        else if (job) runJob(*job);
    }
}

void AnalyseController::post(bool map, bool measure) {
    wantMap_ = wantMap_ || map;
    wantMeasure_ = wantMeasure_ || measure;
    ++gen_;   // anything running is stale now
    debounce_.start();
}

void AnalyseController::runJob(const Job& j) {
    const std::shared_ptr<const Clip>& c = j.clip;
    if (!c) return;
    const std::vector<Onset>& on = j.onsets;
    auto stale = [&] { return gen_.load() != j.gen; };
    double t0 = 0, t1 = 0;
    QString why;
    const bool winOk = measure_window(*c, j.s, on, t0, t1, &why);
    // map
    QImage img;
    std::vector<float> mon, moff;
    PixelSpectra ps;
    QString mapNote;
    bool mapDone = false;
    if (j.map) {
        double m0 = 0, m1 = c->duration;
        if (j.cfg["mapWin"].toString() == "window" && winOk) { m0 = t0; m1 = t1; }
        c->countMaps(m0, m1, mon, moff);
        const QString kind = j.cfg["map"].toString();
        if (kind == "polarity") img = render_polarity(mon, moff, c->W, c->H);
        else if (kind == "freq" || kind == "energy") {
            const double e = std::min(m1, m0 + 1.0);   // per-pixel spectra over at most 1 s (memory)
            if (e < m1) mapNote = QString("frequency map over %1–%2 s (at most 1 s)").arg(m0, 0, 'f', 3).arg(e, 0, 'f', 3);
            ps = pixel_spectra(*c, m0, e, j.s.fs, j.s.toneLo, j.s.toneHi, j.s.bandLo, j.s.bandHi, j.cfg["mapMaxPix"].toInt(), &cancel_);
            if (stale()) return;
            img = kind == "freq" ? render_freq(ps, j.cfg["mapSnr"].toDouble()) : render_energy(ps);
        } else {
            std::vector<float> all(mon.size());
            for (size_t i = 0; i < all.size(); ++i) all[i] = mon[i] + moff[i];
            img = render_log(all, c->W, c->H);
        }
        mapDone = true;
    }
    if (stale()) return;
    MeasureResult res;
    if (j.measure) res = run_measurement(*c, j.rois, j.s, on);
    if (stale()) return;
    Spectrogram2 sg;
    std::vector<float> rate;
    int selIdx = -1;
    for (size_t i = 0; i < j.rois.size(); ++i) if (j.rois[i].id == j.sel) selIdx = (int)i;
    if (selIdx < 0 && !j.rois.empty()) selIdx = 0;
    if (j.sg && selIdx >= 0) sg = roi_spectrogram(*c, j.rois[selIdx], j.s);
    RpmTrack rpmTrack;
    if (j.sg && selIdx >= 0 && j.s.rpmOn) rpmTrack = roi_rpm_track(*c, j.rois[selIdx], j.s);
    if (j.rate && selIdx >= 0) { const RoiPixels px = rasterize(j.rois[selIdx], c->W, c->H); rate = c->roiRate(&px); }
    if (stale()) return;
    const int selId = selIdx >= 0 ? j.rois[selIdx].id : -1;
    QMetaObject::invokeMethod(this, [this, gen = j.gen, mapDone, img, mon = std::move(mon), moff = std::move(moff), ps = std::move(ps), mapNote,
                                     res = std::move(res), sg = std::move(sg), rate = std::move(rate), rpmTrack = std::move(rpmTrack), selId, why, winOk, t0, t1]() mutable {
        if (gen != gen_.load()) return;
        if (mapDone) {
            {
                std::lock_guard<std::mutex> lk(mapmx_);
                map_ = img;
                ++mapSeq_;
            }
            mapOn_ = std::move(mon);
            mapOff_ = std::move(moff);
            ps_ = std::move(ps);
            state_["mapNote"] = mapNote;
            state_["mapFLo"] = ps_.fLo;
            state_["mapFHi"] = ps_.fHi;
            state_["mapPixels"] = ps_.pixels;
        }
        res_ = std::move(res);
        sg_ = std::move(sg);
        rpmTrack_ = std::move(rpmTrack);
        sgRoi_ = selId;
        selRate_ = std::move(rate);
        selRateRoi_ = selId;
        state_["busy"] = false;
        state_["note"] = winOk ? QString() : why;
        state_["t0"] = winOk ? t0 : 0.0;
        state_["t1"] = winOk ? t1 : 0.0;
        state_["winOk"] = winOk;
        rebuildTable();
        emit stateChanged();
        emit resultsChanged();
        emit plotsChanged();
    }, Qt::QueuedConnection);
}

// ---- clip -------------------------------------------------------------------------------------------------------------------
void AnalyseController::openMeasurement(const QString& rawPath, const QString& dir) {
    if (clip_ && clip_->path == rawPath && !state_["loading"].toBool()) { loadMeasurement(dir); return; }
    open(rawPath);
    pendingMeas_ = dir;
}

void AnalyseController::open(const QString& rawPath) {
    if (rawPath.isEmpty()) return;
    pendingMeas_.clear();
    cancel_ = true;
    ++gen_;
    clipInfo_ = QVariantMap{{"path", rawPath}, {"name", QFileInfo(rawPath).completeBaseName()}};
    state_["loading"] = true;
    state_["progress"] = 0.0;
    state_["error"] = QString();
    emit clipChanged();
    emit stateChanged();
    std::lock_guard<std::mutex> lk(jmx_);
    pending_.reset();
    tasks_.push_back([this, rawPath] {
        cancel_ = false;
        auto c = std::make_shared<Clip>();
        QString err;
        double lastP = -1;
        const bool ok = c->load(rawPath, &err, [&](double p) {
            if (p - lastP < 0.05) return;
            lastP = p;
            QMetaObject::invokeMethod(this, [this, p] { state_["progress"] = p; emit stateChanged(); }, Qt::QueuedConnection);
        }, &cancel_);
        std::vector<Onset> on;
        if (ok) on = find_onsets(*c);
        QMetaObject::invokeMethod(this, [this, ok, err, rawPath, c = std::shared_ptr<const Clip>(ok ? c : nullptr), on]() {
            if (clipInfo_["path"].toString() != rawPath) return;   // another clip was opened meanwhile
            state_["loading"] = false;
            if (!ok) {
                if (err != "cancelled") { state_["error"] = err; emit toast(err); }
                emit stateChanged();
                return;
            }
            clip_ = c;
            onsets_ = on;
            const QJsonObject st = c->meta["stats"].toObject();
            clipInfo_ = QVariantMap{{"path", rawPath}, {"name", c->name}, {"duration", c->duration}, {"events", (double)c->size()},
                                    {"width", c->W}, {"height", c->H}, {"triggers", (int)c->triggers.size()},
                                    {"description", c->meta["description"].toString(c->meta["label"].toString())},
                                    {"project", QFileInfo(rawPath).absoluteDir().dirName()}, {"folder", QFileInfo(rawPath).absolutePath()},
                                    {"created", c->meta["created"].toString()}, {"source", c->meta["source"].toString()},
                                    {"biasOn", c->meta["biases"].toObject()["bias_diff_on"].toVariant()},
                                    {"biasOff", c->meta["biases"].toObject()["bias_diff_off"].toVariant()},
                                    {"loaded", true}, {"mb", QFileInfo(rawPath).size() / 1048576.0}};
            if (cfg_["winMode"] == "onset" && onsets_.empty()) cfg_["winMode"] = "abs";
            if (cfg_["onset"].toInt() >= (int)onsets_.size()) cfg_["onset"] = 0;
            cfg_["t0"] = 0.0;
            cfg_["t1"] = 0.0;
            curMeasurement_.clear();
            state_["measurement"] = QString();
            refreshMeasurements();
            emit clipChanged();
            emit cfgChanged();
            emit stateChanged();
            if (!pendingMeas_.isEmpty()) { const QString d = pendingMeas_; pendingMeas_.clear(); loadMeasurement(d); }
            post(true, true);
        }, Qt::QueuedConnection);
    });
    jcv_.notify_one();
}

void AnalyseController::close() {
    cancel_ = true;
    ++gen_;
    clip_.reset();
    onsets_.clear();
    clipInfo_.clear();
    res_ = {};
    sg_ = {};
    {
        std::lock_guard<std::mutex> lk(mapmx_);
        map_ = QImage();
        ++mapSeq_;
    }
    rebuildTable();
    emit clipChanged();
    emit resultsChanged();
    emit plotsChanged();
}

QVariantList AnalyseController::onsets() const {
    QVariantList l;
    for (const Onset& o : onsets_) l << QVariantMap{{"t", o.t}, {"source", o.source}, {"pre", o.pre}};
    return l;
}

bool AnalyseController::takeFrame(QImage& out, uint64_t& seq) {
    std::lock_guard<std::mutex> lk(mapmx_);
    if (seq == mapSeq_ || map_.isNull()) return false;
    out = map_;
    seq = mapSeq_;
    return true;
}

// ---- settings ----------------------------------------------------------------------------------------------------------------
void AnalyseController::setCfg(const QString& key, const QVariant& v) {
    if (!cfg_.contains(key) || cfg_.value(key) == v) return;
    cfg_[key] = v;
    static const QStringList uiOnly = {"tool", "brushR", "compare", "lineRel", "lineMinLen", "blobMinArea", "plot"};
    static const QStringList mapKeys = {"map", "mapWin", "mapSnr", "mapMaxPix"};
    static const QStringList both = {"winMode", "t0", "t1", "skip", "len", "onset", "fs", "toneLo", "toneHi", "bandLo", "bandHi"};
    saveCfg();
    emit cfgChanged();
    if (uiOnly.contains(key)) { emit plotsChanged(); return; }
    const bool mapToo = mapKeys.contains(key) || (both.contains(key) && (cfg_["mapWin"] == "window" || cfg_["map"] == "freq" || cfg_["map"] == "energy"));
    if (clip_) post(mapToo, !mapKeys.contains(key));
}

void AnalyseController::setWindow(double t0, double t1) {
    if (t1 < t0) std::swap(t0, t1);
    cfg_["winMode"] = "abs";
    cfg_["t0"] = std::max(0.0, t0);
    cfg_["t1"] = t1;
    emit cfgChanged();
    if (clip_) post(cfg_["mapWin"] == "window" || cfg_["map"] == "freq" || cfg_["map"] == "energy", true);
}

void AnalyseController::useOnset(int k) {
    if (k < 0 || k >= (int)onsets_.size()) return;
    cfg_["winMode"] = "onset";
    cfg_["onset"] = k;
    saveCfg();
    emit cfgChanged();
    post(cfg_["mapWin"] == "window" || cfg_["map"] == "freq" || cfg_["map"] == "energy", true);
}

void AnalyseController::markDrag(int id, double v) {
    double t0 = state_.value("t0").toDouble(), t1 = state_.value("t1").toDouble();
    if (!state_.value("winOk").toBool() && clip_) { t0 = 0; t1 = clip_->duration; }
    if (id == 1) t0 = std::min(v, t1 - 0.005);
    else if (id == 2) t1 = std::max(v, t0 + 0.005);
    else return;
    setWindow(t0, t1);
}

void AnalyseController::onRoisEdited(bool geometry) {
    if (!clip_) { rebuildTable(); emit plotsChanged(); return; }
    if (geometry) post(false, true);
    else if (sgRoi_ != rs_.selected) post(false, true);   // new selection: its spectrogram and rate
    else { rebuildTable(); emit plotsChanged(); }
    state_["dirty"] = true;
    emit stateChanged();
}

// ---- detection -------------------------------------------------------------------------------------------------------------
void AnalyseController::detect(const QString& kind, bool replace) {
    if (!clip_ || mapOn_.empty()) { emit toast("Open a clip first"); return; }
    std::vector<float> cnt(mapOn_.size());
    for (size_t i = 0; i < cnt.size(); ++i) cnt[i] = mapOn_[i] + mapOff_[i];
    const int W = clip_->W, H = clip_->H;
    const double rel = cfg_["lineRel"].toDouble(), minLen = cfg_["lineMinLen"].toDouble();
    const int minArea = cfg_["blobMinArea"].toInt();
    setStateKey("busy", true);
    std::lock_guard<std::mutex> lk(jmx_);
    tasks_.push_back([this, cnt = std::move(cnt), W, H, kind, replace, rel, minLen, minArea] {
        std::vector<Roi> found;
        if (kind == "blobs") {
            BlobOptions o;
            o.minArea = minArea;
            found = detect_blobs(cnt, W, H, o);
        } else {
            LineOptions o;
            o.rel = rel;
            o.minLen = minLen;
            found = detect_lines(cnt, W, H, o);
        }
        QMetaObject::invokeMethod(this, [this, found = std::move(found), kind, replace]() mutable {
            setStateKey("busy", false);
            if (found.empty()) { emit toast(kind == "blobs" ? "No active regions found in this map" : "No straight lines found in this map"); return; }
            std::vector<Roi> rois = replace ? std::vector<Roi>() : rs_.rois;
            for (Roi& r : found) { r.id = 0; rois.push_back(std::move(r)); }
            setRois(std::move(rois));
            emit toast(QString("%1 %2 found").arg(found.size()).arg(kind == "blobs" ? "regions" : "lines"));
        }, Qt::QueuedConnection);
    });
    jcv_.notify_one();
}

QVariantMap AnalyseController::pixelInfo(double x, double y) const {
    QVariantMap m;
    if (!clip_) return m;
    const int ix = (int)std::floor(x), iy = (int)std::floor(y);
    if (ix < 0 || iy < 0 || ix >= clip_->W || iy >= clip_->H) return m;
    const size_t i = (size_t)iy * clip_->W + ix;
    if (i < mapOn_.size()) { m["on"] = mapOn_[i]; m["off"] = mapOff_[i]; }
    if (i < ps_.freq.size() && ps_.freq[i] == ps_.freq[i]) { m["f"] = ps_.freq[i]; m["snr"] = ps_.snr[i]; }
    return m;
}

// ---- measurements ----------------------------------------------------------------------------------------------------------
void AnalyseController::refreshMeasurements() {
    meas_.clear();
    if (!clip_) { emit measurementsChanged(); return; }
    for (const SavedMeasurement& m : list_measurements(clip_->path)) {
        int ok = 0;
        QStringList tones;
        for (const QJsonValue& v : m.results) {
            const QJsonObject r = v.toObject();
            if (r["ok"].toBool()) { ++ok; tones << QString("%1 %2").arg(r["name"].toString()).arg(r["f_hz"].toDouble(), 0, 'f', 1); }
            if (r["rpm_ok"].toBool()) tones << QString("%1 %2 rpm").arg(r["name"].toString()).arg(r["rpm"].toDouble(), 0, 'f', 1);
        }
        meas_ << QVariantMap{{"name", m.name}, {"dir", m.dir}, {"created", m.created.left(16).replace('T', ' ')}, {"rois", (int)m.rois.size()},
                             {"tones", ok}, {"summary", tones.join(", ")}, {"current", m.name == curMeasurement_}};
    }
    emit measurementsChanged();
}

void AnalyseController::saveMeasurement(const QString& name) {
    if (!clip_) return;
    if (state_["busy"].toBool() || debounce_.isActive()) { emit toast("Still computing: save again in a moment"); return; }
    QImage map;
    {
        std::lock_guard<std::mutex> lk(mapmx_);
        map = map_;
    }
    QString err;
    const QString n = name.trimmed().isEmpty() ? QString("measurement") : name.trimmed();
    if (!save_measurement(clip_->path, n, rs_.rois, settings(), res_, map, &err)) { emit toast(err); return; }
    curMeasurement_ = n;
    state_["measurement"] = n;
    state_["dirty"] = false;
    refreshMeasurements();
    emit stateChanged();
    emit toast("Saved measurement \"" + n + "\" (" + QDir(measure_dir(clip_->path)).filePath(safe_name(n)) + ")");
    emit measurementSaved(clip_->path);
}

void AnalyseController::loadMeasurement(const QString& dir) {
    SavedMeasurement m;
    if (!load_measurement(dir, m)) { emit toast("Cannot read " + dir); return; }
    const QJsonObject s = m.settings.toJson();
    const MeasureSettings& ms = m.settings;
    cfg_["fs"] = ms.fs; cfg_["signal"] = ms.signal; cfg_["window"] = ms.window; cfg_["bandLo"] = ms.bandLo; cfg_["bandHi"] = ms.bandHi;
    cfg_["toneLo"] = ms.toneLo; cfg_["toneHi"] = ms.toneHi; cfg_["df"] = ms.df; cfg_["snrMin"] = ms.snrMin; cfg_["fmax"] = ms.fmax;
    cfg_["commonMode"] = ms.commonMode; cfg_["winMode"] = ms.winMode; cfg_["t0"] = ms.t0; cfg_["t1"] = ms.t1; cfg_["skip"] = ms.skip;
    cfg_["len"] = ms.len; cfg_["onset"] = ms.onset; cfg_["sgWinMs"] = ms.sgWinMs; cfg_["sgHopMs"] = ms.sgHopMs;
    cfg_["rpmOn"] = ms.rpmOn; cfg_["rpmMarks"] = ms.rpmMarks; cfg_["rpmLo"] = ms.rpmLo; cfg_["rpmHi"] = ms.rpmHi; cfg_["rpmWin"] = ms.rpmWin;
    (void)s;
    RoiSet tmp;
    tmp.fromJson(m.rois, clip_ ? clip_->W : 1280, clip_ ? clip_->H : 720);
    curMeasurement_ = m.name;
    state_["measurement"] = m.name;
    emit cfgChanged();
    wantMap_ = true;
    setRois(tmp.rois);
    state_["dirty"] = !(clip_ && m.dir.startsWith(measure_dir(clip_->path)));
    refreshMeasurements();
    emit stateChanged();
}

void AnalyseController::applyRecipe(const QString& dir) { loadMeasurement(dir); }

void AnalyseController::deleteMeasurement(const QString& dir) {
    if (!clip_ || !QDir::cleanPath(dir).startsWith(QDir::cleanPath(measure_dir(clip_->path)))) return;
    QDir(dir).removeRecursively();
    if (QDir(measure_dir(clip_->path)).isEmpty()) QDir().rmdir(measure_dir(clip_->path));
    refreshMeasurements();
    emit measurementSaved(clip_->path);
}

QVariantList AnalyseController::recipes() const {
    QVariantList l;
    if (!clip_) return l;
    const QString folder = QFileInfo(clip_->path).absolutePath();
    QDirIterator it(folder, {"measurement.json"}, QDir::Files, QDirIterator::Subdirectories);
    std::vector<QVariantMap> v;
    while (it.hasNext()) {
        const QString f = it.next();
        const QString dir = QFileInfo(f).absolutePath();
        if (!QFileInfo(dir).absolutePath().endsWith(".measure")) continue;
        SavedMeasurement m;
        if (!load_measurement(dir, m)) continue;
        QString clip = QFileInfo(QFileInfo(dir).absolutePath()).fileName();
        clip.chop(8);
        v.push_back(QVariantMap{{"name", m.name}, {"clip", clip}, {"dir", dir}, {"rois", (int)m.rois.size()}, {"created", m.created},
                                {"window", m.settings.winMode == "onset" ? QString("onset +%1 ms, %2 ms").arg(m.settings.skip * 1000, 0, 'f', 0).arg(m.settings.len * 1000, 0, 'f', 0)
                                                                          : QString("%1–%2 s").arg(m.settings.t0, 0, 'f', 2).arg(m.settings.t1, 0, 'f', 2)}});
    }
    std::sort(v.begin(), v.end(), [](const QVariantMap& a, const QVariantMap& b) { return a["created"].toString() > b["created"].toString(); });
    for (auto& m : v) l << m;
    return l;
}

void AnalyseController::showFolder() const {
    if (!clip_) return;
    const QString d = QDir(measure_dir(clip_->path)).exists() ? measure_dir(clip_->path) : QFileInfo(clip_->path).absolutePath();
    QDesktopServices::openUrl(QUrl::fromLocalFile(d));
}

// ---- results ----------------------------------------------------------------------------------------------------------------
QVariantList AnalyseController::results() const {
    QVariantList l;
    for (const RoiResult& r : res_.rois)
        l << QVariantMap{{"id", r.id}, {"name", r.name}, {"color", r.color}, {"f", r.tone.f}, {"snr", r.tone.snr}, {"amp", r.tone.amp},
                         {"ok", r.ok}, {"events", (double)r.events}, {"decay", std::isfinite(r.decayS) ? r.decayS * 1000 : -1.0},
                         {"rpm", r.rpm.ok ? r.rpm.rpm : -1.0}, {"rpmOk", r.rpm.ok}, {"rpmPass", r.rpm.pass}, {"rpmSnr", r.rpm.snr},
                         {"rpmHarm", r.rpm.found}, {"rpmSpread", r.rpm.spread}, {"rpmNote", r.rpm.note}};
    return l;
}

void AnalyseController::rebuildTable() {
    std::vector<std::vector<hz::Cell>> rows;
    std::vector<int> keys;
    const bool rpm = cfg_["rpmOn"].toBool();
    for (const RoiResult& r : res_.rois) {
        const Roi* g = rs_.find(r.id);
        if (!g) continue;
        const char* ty = g->type == RoiType::Line ? "line" : g->type == RoiType::Rect ? "rect" : g->type == RoiType::Polygon ? "polygon" : "mask";
        std::vector<hz::Cell> row;
        row.push_back({g->name, g->color, (double)r.id});
        (void)ty;
        const bool has = r.tone.f > 0;
        row.push_back({has ? QString::number(r.tone.f, 'f', 2) : QString("–"), r.ok ? QColor("#3fd68a") : QColor(), r.tone.f});
        row.push_back({has ? QString::number(r.tone.snr, 'f', 0) : QString("–"), has && !r.ok ? QColor("#ffc23d") : QColor(), r.tone.snr});
        row.push_back({has ? QString::number(r.tone.amp, 'g', 3) : QString("–"), QColor(), r.tone.amp});
        row.push_back({std::isfinite(r.decayS) ? QString::number(r.decayS * 1000, 'f', 0) : QString("–"), QColor(), std::isfinite(r.decayS) ? r.decayS : 0});
        if (rpm) row.push_back({r.rpm.ok ? QString::number(r.rpm.rpm, 'f', 1) : QString("–"), r.rpm.ok ? QColor("#ff4fb0") : QColor(), r.rpm.ok ? r.rpm.rpm : 0});
        row.push_back({QString::number(r.events), QColor(), (double)r.events, 0, false, true});
        QStringList pk;
        for (size_t k = 0; k < r.peaks.size() && k < 4; ++k) pk << QString("%1/%2").arg(r.peaks[k].f, 0, 'f', 1).arg(r.peaks[k].snr, 0, 'f', 0);
        row.push_back({pk.join("  "), QColor(), 0, 0, false, true});
        rows.push_back(std::move(row));
        keys.push_back(r.id);
    }
    if (rpm)
        table_.set({"ROI", "f (Hz)", "SNR", "Amp", "Decay (ms)", "RPM", "Events", "Peaks (Hz/SNR)"}, std::move(rows), std::move(keys),
                   {56, 76, 50, 62, 78, 76, 80, 240});
    else
        table_.set({"ROI", "f (Hz)", "SNR", "Amp", "Decay (ms)", "Events", "Peaks (Hz/SNR)"}, std::move(rows), std::move(keys),
                   {56, 76, 50, 62, 78, 80, 240});
    table_.setSelectedKey(rs_.selected);
}

// ---- plots ------------------------------------------------------------------------------------------------------------------
void AnalyseController::fillPlot(const QString& kind, hz::PlotData& pd) {
    const bool all = cfg_["compare"] == "all";
    auto shown = [&](const RoiResult& r) { return all || r.id == rs_.selected || (rs_.selected < 0 && &r == &res_.rois.front()); };
    auto colorOf = [&](const RoiResult& r) { const Roi* g = rs_.find(r.id); return g ? g->color : r.color; };
    auto nameOf = [&](const RoiResult& r) { const Roi* g = rs_.find(r.id); return g ? g->name : r.name; };
    const double t0 = state_.value("t0").toDouble(), t1 = state_.value("t1").toDouble();
    if (kind == "ana:rate") {
        pd.xlabel = "time in the clip (s)";
        pd.ylabel = "events (Mev/s)";
        pd.xunit = "s";
        pd.yunit = "Mev/s";
        pd.empty = clip_ ? "–" : "Open a recording from the Library";
        pd.yFromZero = true;
        pd.legend = true;
        if (!clip_) return;
        const auto& r = clip_->rateAll;
        const int n = (int)r.size(), step = std::max(1, n / 2000);
        hz::PlotSeries a{hz::PlotSeries::Line, "all pixels", QColor(150, 158, 170)};
        a.width = 1;
        for (int k = 0; k < n; k += step) {
            float mx = 0;
            for (int j = k; j < std::min(n, k + step); ++j) mx = std::max(mx, r[j]);
            a.x.push_back((k + step / 2.0) * 1e-3);
            a.y.push_back(mx * 1e-3);
        }
        pd.series.push_back(std::move(a));
        if (!selRate_.empty() && rs_.find(selRateRoi_)) {
            const Roi* g = rs_.find(selRateRoi_);
            hz::PlotSeries s{hz::PlotSeries::Line, g->name, g->color};
            s.width = 1.3f;
            const int m = (int)selRate_.size();
            for (int k = 0; k < m; k += step) {
                float mx = 0;
                for (int j = k; j < std::min(m, k + step); ++j) mx = std::max(mx, selRate_[j]);
                s.x.push_back((k + step / 2.0) * 1e-3);
                s.y.push_back(mx * 1e-3);
            }
            pd.series.push_back(std::move(s));
        }
        for (size_t i = 0; i < onsets_.size() && i < 60; ++i) {
            hz::PlotMark m;
            m.v = onsets_[i].t;
            m.color = QColor(255, 194, 61, 150);
            m.label = QString::number(i + 1);
            pd.marks.push_back(m);
        }
        for (const TrigEv& t : clip_->triggers) {
            if (t.value != 1) continue;
            hz::PlotMark m;
            m.v = t.t * 1e-6;
            m.color = QColor(47, 208, 208, 120);
            m.dashed = true;
            pd.marks.push_back(m);
        }
        if (state_.value("winOk").toBool()) {
            hz::PlotMark a0, a1;
            a0.v = t0; a0.color = QColor("#ff4fb0"); a0.width = 2; a0.dragId = 1; a0.label = "from";
            a1.v = t1; a1.color = QColor("#ff4fb0"); a1.width = 2; a1.dragId = 2; a1.label = "to";
            pd.marks.push_back(a0);
            pd.marks.push_back(a1);
        }
    } else if (kind == "ana:spectrum") {
        pd.xlabel = "frequency (Hz)";
        pd.ylabel = "amplitude (rel.)";
        pd.xunit = "Hz";
        pd.empty = !clip_ ? "Open a recording" : rs_.rois.empty() ? "Draw or detect ROIs on the map" : state_.value("note").toString().isEmpty() ? "Computing…" : state_.value("note").toString();
        pd.yFromZero = true;
        pd.fixX = true;
        pd.x0 = 0;
        pd.x1 = settings().fmax;
        for (const RoiResult& r : res_.rois) {
            if (!shown(r) || r.f.empty()) continue;
            hz::PlotSeries s{hz::PlotSeries::Line, nameOf(r), colorOf(r)};
            s.width = r.id == rs_.selected ? 2.0f : 1.2f;
            if (!r.ok) s.color.setAlpha(140);
            s.x = r.f;
            s.y = r.amp;
            pd.series.push_back(std::move(s));
            if (r.ok) pd.texts.push_back({r.tone.f, r.tone.amp, QString("%1 %2 Hz").arg(nameOf(r)).arg(r.tone.f, 0, 'f', 1), colorOf(r)});
        }
        pd.legend = pd.series.size() <= 6;
        for (double f : {settings().toneLo, settings().toneHi}) {
            hz::PlotMark m;
            m.v = f;
            m.color = QColor(76, 155, 255, 150);
            m.dashed = true;
            pd.marks.push_back(m);
        }
        for (double c : res_.common) {
            hz::PlotMark m;
            m.v = c;
            m.color = QColor(200, 200, 200, 120);
            m.dashed = true;
            m.label = "common";
            pd.marks.push_back(m);
        }
    } else if (kind == "ana:signal") {
        pd.xlabel = "time from the window start (ms)";
        pd.ylabel = settings().signal == "displacement" ? "integrated ON−OFF, band-passed (rel.)" : "ON−OFF per bin (rel.)";
        pd.xunit = "ms";
        pd.empty = rs_.rois.empty() ? "Draw or detect ROIs on the map" : "–";
        const double fs = settings().fs;
        for (const RoiResult& r : res_.rois) {
            if (!shown(r) || r.sig.empty()) continue;
            hz::PlotSeries s{hz::PlotSeries::Line, nameOf(r), colorOf(r)};
            s.width = 1;
            const int n = (int)r.sig.size(), step = std::max(1, n / 3000);
            for (int k = 0; k < n; k += step) {
                double lo = r.sig[k], hi = r.sig[k];
                for (int j = k; j < std::min(n, k + step); ++j) { lo = std::min(lo, r.sig[j]); hi = std::max(hi, r.sig[j]); }
                s.x.push_back(k / fs * 1000); s.y.push_back(lo);
                if (step > 1) { s.x.push_back(k / fs * 1000); s.y.push_back(hi); }
            }
            pd.series.push_back(std::move(s));
        }
        pd.legend = pd.series.size() <= 6;
    } else if (kind == "ana:rpm") {
        pd.xlabel = "time in the clip (s)";
        pd.ylabel = "speed (rpm)";
        pd.xunit = "s";
        pd.yunit = "rpm";
        pd.legend = false;
        pd.empty = !cfg_["rpmOn"].toBool() ? "Switch RPM on (side panel)" : rs_.rois.empty() ? "Select a ROI that the rotating part passes" : "No speed found in the range";
        if (!cfg_["rpmOn"].toBool() || rpmTrack_.t.empty()) return;
        const Roi* g = rs_.find(sgRoi_);
        hz::PlotSeries line{hz::PlotSeries::Line, g ? g->name : QString("rpm"), g ? g->color : QColor("#ff4fb0")};
        line.width = 1.6f;
        hz::PlotSeries dots{hz::PlotSeries::Scatter, "", line.color};
        dots.size = 3.f;
        bool any = false;
        for (size_t i = 0; i < rpmTrack_.t.size(); ++i) {
            if (!std::isfinite(rpmTrack_.rpm[i])) continue;
            line.x.push_back(rpmTrack_.t[i]);
            line.y.push_back(rpmTrack_.rpm[i]);
            any = true;
        }
        if (!any) return;
        dots.x = line.x;
        dots.y = line.y;
        pd.series.push_back(std::move(line));
        pd.series.push_back(std::move(dots));
        for (const RoiResult& r : res_.rois)
            if (r.id == sgRoi_ && r.rpm.ok) {
                hz::PlotMark m;
                m.v = r.rpm.rpm;
                m.horizontal = true;
                m.color = QColor(255, 79, 176, 120);
                m.dashed = true;
                m.label = QString("%1 rpm (window)").arg(r.rpm.rpm, 0, 'f', 1);
                pd.marks.push_back(m);
            }
        if (state_.value("winOk").toBool())
            for (double v : {t0, t1}) {
                hz::PlotMark m;
                m.v = v;
                m.color = QColor("#ff4fb0");
                m.width = 1.5f;
                pd.marks.push_back(m);
            }
    } else if (kind == "ana:sg") {
        pd.xlabel = "time in the clip (s)";
        pd.ylabel = "frequency (Hz)";
        pd.xunit = "s";
        pd.yunit = "Hz";
        pd.legend = false;
        pd.empty = rs_.rois.empty() ? "Select a ROI for its spectrogram" : "–";
        if (sg_.nt < 2 || sg_.nf < 2) return;
        // dB image, 50 dB range under the maximum
        float mx = 1e-20f;
        for (float v : sg_.amp) mx = std::max(mx, v);
        QImage img(sg_.nt, sg_.nf, QImage::Format_RGB32);
        for (int k = 0; k < sg_.nf; ++k) {
            QRgb* row = reinterpret_cast<QRgb*>(img.scanLine(sg_.nf - 1 - k));
            for (int i = 0; i < sg_.nt; ++i) {
                const float db = 20 * std::log10(std::max(sg_.amp[(size_t)i * sg_.nf + k], 1e-20f) / mx);
                row[i] = hz::viridis(std::clamp((db + 50) / 50, 0.f, 1.f));
            }
        }
        pd.heat.img = img;
        pd.heat.x0 = sg_.t.front();
        pd.heat.x1 = sg_.t.back();
        pd.heat.y0 = sg_.f.front();
        pd.heat.y1 = sg_.f.back();
        pd.heat.valid = true;
        if (!sg_.trackT.empty()) {
            hz::PlotSeries s{hz::PlotSeries::Scatter, "tone track", QColor(255, 255, 255, 170)};
            s.size = 2.5f;
            s.x = sg_.trackT;
            s.y = sg_.trackF;
            pd.series.push_back(std::move(s));
        }
        if (state_.value("winOk").toBool())
            for (double v : {t0, t1}) {
                hz::PlotMark m;
                m.v = v;
                m.color = QColor("#ff4fb0");
                m.width = 1.5f;
                pd.marks.push_back(m);
            }
    }
}

}  // namespace evs

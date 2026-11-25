#include "controller.hpp"
#include "prof.hpp"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QTextStream>
#include <algorithm>
#include <cmath>
#include "dsp.hpp"
#include "pixelmodel.hpp"

namespace evs {

static const int TICK_MS = 33;
static const double TRACE_S = 0.3;
static const double LINK_BPS = 2.5e9 / 8;   // 2.5GigE payload ceiling (approx.)

static QString refPath() {
    const QString d = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(d);
    return QDir(d).filePath("reference.png");
}

// ---- construction ----------------------------------------------------------------------------------------------------
EvsController::EvsController(const QString& source, QObject* parent) : RoiController(parent) {
    st_ = QVariantMap{
        {"page", "library"}, {"source", source}, {"streamFmt", "encoded"},
        {"viewMode", 0}, {"decayMs", 8.0}, {"fps", 60.0}, {"tool", "select"}, {"brushR", 6.0},
        {"preset", "custom"},
        {"bias_diff_on", 30.0}, {"bias_diff_off", 30.0}, {"bias_fo", -10.0}, {"bias_hpf", 80.0}, {"bias_refr", 0.0},
        {"erc_enable", false}, {"erc_rate", 60.0},
        {"afk_enable", true}, {"afk_low", 50.0}, {"afk_high", 99.0}, {"trail_enable", false},
        {"roiOn", false}, {"roiHw", true}, {"roiX", 0.0}, {"roiY", 0.0}, {"roiW", 1280.0}, {"roiH", 720.0},
        {"liveSignal", "velocity"}, {"bandLo", 100.0}, {"bandHi", 1000.0}, {"toneLo", 150.0}, {"toneHi", 500.0},
        {"liveWin", 0.6}, {"specMin", 0.0}, {"specMax", 1200.0}, {"snrMin", 8.0},
        {"project", ""}, {"recName", "rec"}, {"recDesc", ""}, {"recLen", 2.0}, {"preMs", 200.0}, {"trigFactor", 4.0},
        {"amOn", false}, {"amSource", 0}, {"amRegion", false}, {"amFactor", 4.0}, {"amGap", 0.15}, {"amSkipMs", 8.0}, {"amLenMs", 50.0},
        {"amSnr", 8.0}, {"amWindow", "rect"}, {"amSignal", "velocity"}, {"amSave", false}, {"amSaveLen", 0.5},
        {"webOn", true}, {"webPort", 8080}, {"chartRays", 36}, {"chartHz", 4.0}, {"chartInvert", true}, {"refOn", false}, {"refOpacity", 0.45}, {"autoNoiseKev", 20.0},
        {"simTapEvery", 4.0}, {"simNoise", 0.05}, {"simRock", 1.5}, {"scale", 0.10}, {"contrast", 2.0},
        {"simFan", true}, {"simFanRpm", 600.0}, {"simBlades", 3}, {"simTape", true},
        {"rpmOn", false}, {"rpmMarks", 1}, {"rpmLo", 60.0}, {"rpmHi", 12000.0}, {"rpmWin", 1.0}, {"rpmHistS", 60.0}};
    load();
    if (!source.isEmpty()) st_["source"] = source;
#ifdef EVS_NO_SIM
    st_["source"] = "camera";   // release editions have no simulator
#else
    if (st_["source"].toString().isEmpty()) st_["source"] = "sim";
#endif
    st_["page"] = "library";
    st_["amOn"] = false;   // never start measuring on its own
    static const QStringList tools = {"select", "line", "rect", "polygon", "brush", "erase", "pan", "crop"};
    if (!tools.contains(st_["tool"].toString())) st_["tool"] = "select";
    ref_ = QImage(refPath());
    engine_ = std::make_unique<Engine>(1280, 720);
    engine_->setView(st_["viewMode"].toInt(), d("decayMs"), st_["page"] == "live" ? d("fps") : 5.0);
    pushRois();
    rebuildOverlay();
    engine_->setSoftRoi(b("roiOn"), QRectF(d("roiX"), d("roiY"), d("roiW"), d("roiH")));
    am_table_.set({"#", "Time", "ROI", "f (Hz)", "SNR", "Other ROIs", "Clip"}, {}, {}, {44, 96, 70, 80, 56, 220, 200});
#ifdef EVS_VIEWER
    // viewer edition: no camera, no live picture, no web view; the controller only carries the shared app state
    live_["source"] = QString();
    return;
#endif
    web_ = std::make_unique<WebView>(engine_.get());
    applyWeb();
    lw_th_ = std::thread([this] { liveLoop(); });
    pushLiveJob();
    connect(&tick_, &QTimer::timeout, this, &EvsController::tick);
    tick_.start(TICK_MS);
    connectSource(st_["source"].toString());
}

EvsController::~EvsController() {
    tick_.stop();
    {
        std::lock_guard<std::mutex> lk(lw_mx_);
        lw_quit_ = true;
    }
    lw_cv_.notify_all();
    if (lw_th_.joinable()) lw_th_.join();
    web_.reset();   // before the engine it reads pictures from
    save();
    if (src_) src_->stop();
    src_.reset();
    engine_.reset();
}

void EvsController::save() const {
    QSettings s;
    QVariantMap m = st_;
    m.remove("page");
    s.setValue("evs/st", m);
    s.setValue("evs/rois", QString::fromUtf8(QJsonDocument(rs_.toJson()).toJson(QJsonDocument::Compact)));
}

void EvsController::load() {
    QSettings s;
    const QVariantMap m = s.value("evs/st").toMap();
    for (auto it = m.begin(); it != m.end(); ++it)
        if (st_.contains(it.key())) st_[it.key()] = it.value();
    const QJsonArray a = QJsonDocument::fromJson(s.value("evs/rois").toString().toUtf8()).array();
    rs_.fromJson(a, 1280, 720);
    if (!s.value("evs/presetsSeeded").toBool()) {
        // a starting point for vibration in the 100-1000 Hz band: sensitive, hardware band-stop under 100 Hz
        s.setValue("evs/presets/Vibration 100-1000 Hz",
                   QVariantMap{{"bias_diff_on", -40.0}, {"bias_diff_off", -20.0}, {"bias_fo", -10.0}, {"bias_hpf", 80.0}, {"bias_refr", 0.0},
                               {"erc_enable", false}, {"erc_rate", 60.0}, {"afk_enable", true}, {"afk_low", 50.0}, {"afk_high", 99.0},
                               {"trail_enable", false}});
        s.setValue("evs/presetsSeeded", true);
    }
}

// ---- source ----------------------------------------------------------------------------------------------------------
void EvsController::connectSource(const QString& kindIn) {
#ifdef EVS_VIEWER
    Q_UNUSED(kindIn);
    return;
#endif
#ifdef EVS_NO_SIM
    const QString kind = "camera";
    Q_UNUSED(kindIn);
#else
    const QString& kind = kindIn;
#endif
    if (src_) src_->stop();
    src_.reset();
    engine_->reset();
    feat_err_.clear();
    st_["source"] = kind;
    live_["connected"] = false;
    live_["running"] = false;
    if (kind == "camera") {
        src_ = make_arena_source(st_["streamFmt"].toString() == "decoded");
        if (!src_) { live_["error"] = "This build has no Arena SDK support"; emit toast(live_["error"].toString()); emit stChanged(); emit liveChanged(); return; }
    } else {
#ifndef EVS_NO_SIM
        src_ = make_sim_source();
#endif
    }
    Engine* eng = engine_.get();
    src_->sink = [eng](Chunk&& c) { eng->push(std::move(c)); };
    QString err;
    if (!src_->open(&err)) {
        live_["error"] = err;
        emit toast(err);
        src_.reset();
        emit stChanged();
        emit liveChanged();
        emit featuresChanged();
        return;
    }
    live_["error"] = QString();
    live_["connected"] = true;
    {
        QSettings s;   // node names chosen by hand on the camera page
        s.beginGroup("evs/override");
        for (const QString& k : s.childKeys()) src_->setNodeOverride(k, s.value(k).toString());
    }
    live_["source"] = src_->description();
    engine_->setEncoded(src_->encoded());
    if (src_->kind() == "sim") {
        src_->setSimParam("scale", d("scale"));
        src_->setSimParam("contrast", d("contrast"));
        src_->setSimParam("tapEvery", d("simTapEvery"));
        src_->setSimParam("noiseHz", d("simNoise"));
        src_->setSimParam("rock", d("simRock"));
        src_->setSimParam("fan", b("simFan"));
        src_->setSimParam("fanRpm", d("simFanRpm"));
        src_->setSimParam("blades", st_["simBlades"].toInt());
        src_->setSimParam("tape", b("simTape"));
    }
    reapplyAll();
    applyMeasureTrigger();
    if (!src_->start(&err)) { emit toast(err); live_["error"] = err; }
    live_["running"] = src_->running();
    emit stChanged();
    emit liveChanged();
}

void EvsController::toggleRun() {
    if (!src_) { connectSource(st_["source"].toString()); return; }
    if (src_->running()) src_->stop();
    else {
        engine_->reset();
        QString err;
        if (!src_->start(&err)) emit toast(err);
    }
    live_["running"] = src_->running();
    emit liveChanged();
}

void EvsController::reapplyAll() {
    if (!src_) return;
    feat_err_.clear();
    for (const QString& k : {"bias_diff_on", "bias_diff_off", "bias_fo", "bias_hpf", "bias_refr", "erc_rate", "erc_enable",
                             "afk_low", "afk_high", "afk_enable", "trail_enable"})
        applyFeature(k);
    applyCropHw();
    emit featuresChanged();
}

void EvsController::applyFeature(const QString& key) {
    if (!src_) return;
    QString err;
    bool ok = true;
    if (key.startsWith("afk")) {
        // the IMX636 anti-flicker works on 50..500 Hz: used as a band-STOP
        const double lo = std::clamp(d("afk_low"), 50.0, 499.0), hi = std::clamp(d("afk_high"), lo + 1, 500.0);
        if (b("afk_enable")) {
            src_->setFeature("afk_enable", false, nullptr);   // band changes need the filter off
            src_->setFeature("afk_mode", "BandStop", &err);
            ok = src_->setFeature("afk_low", lo, &err) && ok;
            ok = src_->setFeature("afk_high", hi, &err) && ok;
            ok = src_->setFeature("afk_enable", true, &err) && ok;
        } else {
            ok = src_->setFeature("afk_enable", false, &err);
        }
        if (ok) feat_err_.remove("afk"); else feat_err_["afk"] = err;
        return;
    }
    const QVariant v = st_.value(key);
    ok = src_->setFeature(key, v, &err);
    if (ok) feat_err_.remove(key); else feat_err_[key] = err;
}

// ---- noise-floor finder: with a STILL scene, walk both thresholds from insensitive to sensitive until the event rate
//      (= noise, nothing moves) passes the limit, and keep the last quiet level. ------------------------------------------
namespace {
constexpr int AUTO_SETTLE = 15, AUTO_MEAS = 24;       // ticks (33 ms): let the camera settle, then average the rate
constexpr double AUTO_ON_HI = 120, AUTO_STEP = 4;     // start (least sensitive; the ON range ends at 152) and step of the ON threshold
constexpr double ON_MIN = -88, OFF_MIN = -38;         // IMX636 bias ranges
}

void EvsController::autoSensitivity() {
    if (auto_.on) { finishAuto("cancelled: biases restored", true); return; }
    if (!src_ || !src_->running()) { emit toast("Start the camera first"); return; }
    auto_ = AutoSens{};
    auto_.on = true;
    auto_.on0 = d("bias_diff_on");
    auto_.off0 = d("bias_diff_off");
    auto_.gap = auto_.off0 - auto_.on0;      // keep the user's ON/OFF difference
    auto_.limitKev = d("autoNoiseKev");
    applyAutoLevel(0);
}

void EvsController::applyAutoLevel(int k) {
    auto_.k = k;
    auto_.ticks = 0; auto_.sum = 0; auto_.n = 0;
    const double on = AUTO_ON_HI - AUTO_STEP * k;
    setv("bias_diff_on", on);
    setv("bias_diff_off", on + auto_.gap);
    live_["autoText"] = QString("Looking for the noise floor: ON %1 / OFF %2 …").arg(on).arg(on + auto_.gap);
}

void EvsController::finishAuto(const QString& text, bool restore) {
    if (restore) { setv("bias_diff_on", auto_.on0); setv("bias_diff_off", auto_.off0); }
    auto_.on = false;
    live_["autoText"] = text;
    emit toast(text);
}

void EvsController::autoStep() {
    if (!src_ || !src_->running()) { finishAuto("stopped: no event stream", true); return; }
    if (auto_.ticks < AUTO_SETTLE) { ++auto_.ticks; return; }
    if (auto_.ticks < AUTO_SETTLE + AUTO_MEAS) { auto_.sum += live_.value("rate").toDouble() / 1e3; ++auto_.n; ++auto_.ticks; return; }
    const double kev = auto_.sum / std::max(1, auto_.n);
    const double on = AUTO_ON_HI - AUTO_STEP * auto_.k;
    if (kev > auto_.limitKev) {
        if (auto_.k == 0) { finishAuto(QString("Noisy even at the least sensitive setting (%1 kev/s): is the scene still? Raise the noise limit or the high-pass.").arg(kev, 0, 'f', 0), true); return; }
        applyAutoLevel(auto_.k - 1);   // last quiet level
        auto_.on = false;
        const double qon = AUTO_ON_HI - AUTO_STEP * (auto_.k);
        finishAuto(QString("Most sensitive quiet setting: ON %1 / OFF %2 (one step more sensitive gave %3 kev/s, limit %4). Fine-tune with the sliders.")
                       .arg(qon).arg(qon + auto_.gap).arg(kev, 0, 'f', 0).arg(auto_.limitKev, 0, 'f', 0), false);
        return;
    }
    auto_.lastKev = kev;
    const double nextOn = on - AUTO_STEP;
    if (nextOn < ON_MIN || nextOn + auto_.gap < OFF_MIN) {
        finishAuto(QString("Reached the most sensitive setting (ON %1 / OFF %2) and it is still quiet (%3 kev/s).").arg(on).arg(on + auto_.gap).arg(kev, 0, 'f', 0), false);
        return;
    }
    applyAutoLevel(auto_.k + 1);
}

void EvsController::applyCropHw() {
    if (!src_) return;
    QString err;
    bool ok = true;
    if (b("roiOn") && b("roiHw")) {
        src_->setFeature("roi_enable", false, nullptr);
        ok = src_->setFeature("roi_x", std::floor(d("roiX")), &err) && ok;
        ok = src_->setFeature("roi_y", std::floor(d("roiY")), &err) && ok;
        ok = src_->setFeature("roi_w", std::ceil(d("roiW")), &err) && ok;
        ok = src_->setFeature("roi_h", std::ceil(d("roiH")), &err) && ok;
        ok = src_->setFeature("roi_enable", true, &err) && ok;
    } else {
        src_->setFeature("roi_enable", false, nullptr);
    }
    if (ok) feat_err_.remove("roi"); else feat_err_["roi"] = err;
}

// ---- settings ----------------------------------------------------------------------------------------------------------
void EvsController::setv(const QString& key, const QVariant& v) {
    if (st_.value(key) == v) return;
    st_[key] = v;
    if (key.startsWith("bias_") || key.startsWith("erc_") || key.startsWith("afk_") || key == "trail_enable") {
        st_["preset"] = "custom";
        applyFeature(key);
        emit featuresChanged();
    } else if (key == "roiOn" || key == "roiHw") {
        applyCropHw();
        engine_->setSoftRoi(b("roiOn"), QRectF(d("roiX"), d("roiY"), d("roiW"), d("roiH")));
        emit featuresChanged();
    } else if (key == "viewMode" || key == "decayMs" || key == "fps" || key == "page") {
        // the live picture is only drawn at full rate while the Live page shows it (the web view gets 5 fps then)
        engine_->setView(st_["viewMode"].toInt(), d("decayMs"), st_["page"] == "live" ? d("fps") : 5.0);
    } else if (key == "simTapEvery" && src_) src_->setSimParam("tapEvery", v);
    else if (key == "simNoise" && src_) src_->setSimParam("noiseHz", v);
    else if (key == "simRock" && src_) src_->setSimParam("rock", v);
    else if (key == "simFan" && src_) src_->setSimParam("fan", v);
    else if (key == "simFanRpm" && src_) src_->setSimParam("fanRpm", v);
    else if (key == "simBlades" && src_) src_->setSimParam("blades", v);
    else if (key == "simTape" && src_) src_->setSimParam("tape", v);
    else if (key.startsWith("rpm") && key != "rpmHistS") { rpm_hist_.clear(); live_rpm_ = RpmResult(); }
    else if (key == "source") {
        connectSource(v.toString());
    } else if (key == "streamFmt" && st_["source"] == "camera") {
        connectSource("camera");
    } else if (key.startsWith("am")) {
        applyMeasureTrigger();
    } else if (key == "webOn" || key == "webPort") {
        applyWeb();
    } else if (key == "refOn" || key == "refOpacity") {
        rebuildOverlay();
    }
    save();
    pushLiveJob();
    emit stChanged();
    emit plotsChanged();
}

void EvsController::applyMeasureTrigger() {
    engine_->setMeasureTrigger(b("amOn"), st_["amSource"].toInt(), d("amFactor"), d("amGap"), b("amRegion"));
    if (!b("amOn")) am_pending_.clear();
}

QStringList EvsController::presets() const {
    QSettings s;
    s.beginGroup("evs/presets");
    QStringList l = s.childKeys();
    l.sort(Qt::CaseInsensitive);
    l.prepend("Factory");
    return l;
}

void EvsController::applyPreset(const QString& name) {
    QVariantMap p;
    if (name == "Factory") {
        p = {{"bias_diff_on", 0.0}, {"bias_diff_off", 0.0}, {"bias_fo", 0.0}, {"bias_hpf", 0.0}, {"bias_refr", 0.0},
             {"erc_enable", false}, {"afk_enable", false}, {"trail_enable", false}};
    } else {
        QSettings s;
        p = s.value("evs/presets/" + name).toMap();
        if (p.isEmpty()) return;
    }
    for (auto it = p.begin(); it != p.end(); ++it) st_[it.key()] = it.value();
    st_["preset"] = name;
    reapplyAll();
    save();
    emit stChanged();
    emit plotsChanged();
    emit toast("Preset \"" + name + "\" sent to the camera");
}

void EvsController::savePreset(const QString& name) {
    const QString n = name.trimmed();
    if (n.isEmpty() || n == "Factory" || n.contains('/')) { emit toast("Choose another name"); return; }
    QVariantMap p;
    for (const char* k : {"bias_diff_on", "bias_diff_off", "bias_fo", "bias_hpf", "bias_refr", "erc_enable", "erc_rate", "afk_enable",
                          "afk_low", "afk_high", "trail_enable"})
        p[k] = st_[k];
    QSettings().setValue("evs/presets/" + n, p);
    st_["preset"] = n;
    emit presetsChanged();
    emit stChanged();
    emit toast("Preset \"" + n + "\" saved");
}

void EvsController::deletePreset(const QString& name) {
    QSettings().remove("evs/presets/" + name);
    if (st_["preset"] == name) st_["preset"] = "custom";
    emit presetsChanged();
    emit stChanged();
}

QVariantList EvsController::features() const {
    static const QList<QPair<QString, QString>> labels = {
        {"bias_diff_on", "ON threshold (diff_on)"}, {"bias_diff_off", "OFF threshold (diff_off)"}, {"bias_fo", "Low-pass (fo)"},
        {"bias_hpf", "High-pass (hpf)"}, {"bias_refr", "Refractory (refr)"}, {"erc_enable", "Event rate control"},
        {"erc_rate", "ERC limit"}, {"afk_enable", "Anti-flicker"}, {"afk_mode", "Anti-flicker mode"},
        {"afk_low", "Anti-flicker low"}, {"afk_high", "Anti-flicker high"}, {"trail_enable", "Trail filter"},
        {"roi_enable", "ROI enable"}, {"roi_x", "ROI x"}, {"roi_y", "ROI y"}, {"roi_w", "ROI width"}, {"roi_h", "ROI height"}};
    QVariantList l;
    for (const auto& kv : labels) {
        QVariantMap m;
        m["key"] = kv.first;
        m["label"] = kv.second;
        m["node"] = src_ ? src_->nodeFor(kv.first) : QString();
        QVariant v;
        m["value"] = src_ && src_->getFeature(kv.first, v) ? v.toString() : QString();
        const QString group = kv.first.startsWith("afk") ? "afk" : kv.first.startsWith("roi") ? "roi" : kv.first;
        m["error"] = feat_err_.value(group);
        l << m;
    }
    return l;
}

// ---- ROIs ---------------------------------------------------------------------------------------------------------------
void EvsController::pushRois() {
    std::vector<uint16_t> lut;
    std::vector<int8_t> sign;
    build_lut(rs_.rois, 1280, 720, lut, sign);
    engine_->setRois(std::move(lut), std::move(sign), (int)rs_.rois.size());
    live_sig_.assign(rs_.rois.size(), {});
    live_res_.assign(rs_.rois.size(), {});
    am_pending_.clear();
    ++roi_gen_;
    pushLiveJob();
}

void EvsController::onRoisEdited(bool geometry) {
    if (geometry) pushRois();
    else pushLiveJob();   // the signal type may have changed
    save();
    emit tonesChanged();
    emit plotsChanged();
}

void EvsController::decorateOverlay(QImage& img) {
    if (!b("refOn") || ref_.isNull()) return;
    QPainter p(&img);
    p.setOpacity(std::clamp(d("refOpacity"), 0.05, 1.0));
    p.drawImage(QRectF(0, 0, img.width(), img.height()), ref_);
}

void EvsController::captureReference() {
    QImage img;
    uint64_t seq = 0;
    if (!engine_->frame(img, seq)) { emit toast("No picture yet"); return; }
    ref_ = img.copy();
    ref_.save(refPath());
    st_["refOn"] = true;
    rebuildOverlay();
    save();
    emit stChanged();
    emit toast("Reference picture stored: it stays until you clear it, also after a restart");
}

void EvsController::clearReference() {
    ref_ = QImage();
    QFile::remove(refPath());
    st_["refOn"] = false;
    rebuildOverlay();
    save();
    emit stChanged();
}

void EvsController::loadRoisFrom(const QString& dir) {
    SavedMeasurement m;
    if (!load_measurement(dir, m)) { emit toast("No measurement in " + dir); return; }
    RoiSet tmp;
    tmp.fromJson(m.rois, 1280, 720);
    st_["liveSignal"] = m.settings.signal;
    st_["bandLo"] = m.settings.bandLo;
    st_["bandHi"] = m.settings.bandHi;
    st_["toneLo"] = m.settings.toneLo;
    st_["toneHi"] = m.settings.toneHi;
    st_["snrMin"] = m.settings.snrMin;
    st_["amSkipMs"] = m.settings.skip * 1000;
    st_["amLenMs"] = m.settings.len * 1000;
    st_["amWindow"] = m.settings.window;
    st_["amSignal"] = m.settings.signal;
    setRois(tmp.rois);
    save();
    emit stChanged();
    emit toast(QString("%1 ROIs and the settings of \"%2\" loaded").arg(tmp.rois.size()).arg(m.name));
}

// centre a line ROI on the edges that fire: the recently active pixels across the line
void EvsController::snapRoi(int id) {
    Roi* r = rs_.find(id);
    if (!r || r->type != RoiType::Line || r->pts.size() < 2) { emit toast("Snap works on line ROIs"); return; }
    const double L = r->length();
    const double x0 = r->pts[0].x(), y0 = r->pts[0].y();
    const double ux = (r->pts[1].x() - x0) / L, uy = (r->pts[1].y() - y0) / L, nx = -uy, ny = ux;
    const double R = std::max(20.0, 3 * r->half);
    const int nb = (int)(2 * R) + 1;
    std::vector<double> prof(nb, 0.0);
    QImage img;
    uint64_t seq = 0;
    if (!engine_->frame(img, seq)) return;
    const QRgb bg = qRgb(17, 19, 23);
    for (double s = 0; s <= L; s += 1.0)
        for (int k = 0; k < nb; ++k) {
            const double off = -R + k;
            const int x = (int)std::floor(x0 + ux * s + nx * off), y = (int)std::floor(y0 + uy * s + ny * off);
            if (x < 0 || y < 0 || x >= img.width() || y >= img.height()) continue;
            if (img.pixel(x, y) != bg) prof[k] += 1;
        }
    double tot = 0, mom = 0, mx = 0;
    for (int k = 0; k < nb; ++k) { tot += prof[k]; mom += prof[k] * (-R + k); mx = std::max(mx, prof[k]); }
    if (tot <= 0) { emit toast("No events near this line yet: excite it, then snap"); return; }
    const double c = mom / tot;
    double reach = 0;
    for (int k = 0; k < nb; ++k) if (prof[k] > 0.25 * mx) reach = std::max(reach, std::fabs(-R + k - c));
    for (QPointF& p : r->pts) p += QPointF(nx * c, ny * c);
    r->half = std::clamp(reach + 2.0, 3.0, 60.0);
    edited(true);
}

void EvsController::setCrop(double x0, double y0, double x1, double y1) {
    double a = std::clamp(std::min(x0, x1), 0.0, 1279.0), b2 = std::clamp(std::min(y0, y1), 0.0, 719.0);
    double c = std::clamp(std::max(x0, x1), a + 8, 1280.0), e = std::clamp(std::max(y0, y1), b2 + 8, 720.0);
    st_["roiX"] = std::floor(a);
    st_["roiY"] = std::floor(b2);
    st_["roiW"] = std::ceil(c - a);
    st_["roiH"] = std::ceil(e - b2);
    st_["roiOn"] = true;
    applyCropHw();
    engine_->setSoftRoi(true, QRectF(d("roiX"), d("roiY"), d("roiW"), d("roiH")));
    save();
    emit stChanged();
    emit featuresChanged();
}

void EvsController::clearCrop() {
    st_["roiOn"] = false;
    applyCropHw();
    engine_->setSoftRoi(false, QRectF());
    save();
    emit stChanged();
    emit featuresChanged();
}

// ---- live loop ------------------------------------------------------------------------------------------------------------
void EvsController::tick() {
    hz::ProfScope prof_("live.tick");
    ++tick_n_;
    const QVariantMap es = engine_->stats();
    for (auto it = es.begin(); it != es.end(); ++it) live_[it.key()] = it.value();
    if (src_) {
        const QVariantMap ss = src_->stats();
        for (auto it = ss.begin(); it != ss.end(); ++it) live_[it.key()] = it.value();
        live_["running"] = src_->running();
        double link = ss.value("linkBps").toDouble();
        if (!(link > 0)) link = ss.value("hostBps").toDouble();   // Linux Arena has no StreamEvs* nodes: use the measured host rate
        live_["linkPct"] = std::isfinite(link) ? 100 * link / LINK_BPS : 0.0;
    }
    if (auto_.on) autoStep();
    // recording state
    const RecStatus rs = engine_->recStatus();
    static const char* names[] = {"Ready", "Armed: waiting for the trigger", "Recording", "Writing"};
    static const char* colors[] = {"#6e7580", "#ffc23d", "#ff4f4f", "#4c9bff"};
    live_["recState"] = rs.state;
    rec_state_ = rs.state;
    live_["recText"] = rs.state == 2 ? QString("Recording %1 s").arg(rs.seconds, 0, 'f', 2) : QString(names[std::clamp(rs.state, 0, 3)]);
    live_["recColor"] = colors[std::clamp(rs.state, 0, 3)];
    live_["recSeconds"] = rs.seconds;
    live_["recMB"] = rs.bytes / 1048576.0;
    if (!rs.error.isEmpty()) emit toast(rs.error);
    if (rs.finished) finishRecording(rs);
    if (b("amOn")) processAutoMeasure();
    live_["amPending"] = (int)am_pending_.size();

    engine_->rateWindow(5.0, rate_all_, rate_roi_);
    bool spectraNew = false;
    pullLive(spectraNew);
    // status text at 10 Hz (every binding on app.live re-runs on liveChanged), state changes at once
    if (tick_n_ % 3 == 0 || rs.state != last_rec_state_ || rs.finished) emit liveChanged();
    last_rec_state_ = rs.state;
    if (spectraNew) {
        // only when a shown value changes: the ROI labels on the view repaint on tonesChanged
        QString sig;
        for (const QVariant& v : liveTones()) {
            const QVariantMap m = v.toMap();
            sig += QString("%1:%2:%3:%4;").arg(m["id"].toInt()).arg(m["f"].toDouble(), 0, 'f', 1).arg(m["snr"].toDouble(), 0, 'f', 0).arg(m["ok"].toBool());
        }
        if (sig != tones_sig_) { tones_sig_ = sig; emit tonesChanged(); }
    }
    if (web_ && web_->running()) {
        live_["webViewers"] = web_->viewers();
        if (tick_n_ % 6 == 0 && web_->wanted()) web_->setJson(webJson());
    }
    emit plotsChanged();
}

// ---- web live view ------------------------------------------------------------------------------------------------------------
void EvsController::applyWeb() {
    if (!web_) return;   // viewer edition
    if (!web_) return;
    const int port = std::clamp(st_.value("webPort").toInt(), 1024, 65535);
    if (!b("webOn")) {
        web_->stop();
        live_["webUrls"] = QStringList();
        live_["webError"] = QString();
    } else if (!web_->running() || web_->port() != port) {
        QString err;
        if (web_->start(port, &err)) {
            live_["webUrls"] = web_->urls();
            live_["webError"] = QString();
        } else {
            live_["webUrls"] = QStringList();
            live_["webError"] = QString("Port %1: %2").arg(port).arg(err);
        }
    }
    emit liveChanged();
}

// status, ROIs (geometry + tone) and the live spectra in the plotted range, max-decimated to <= 600 points per ROI
QByteArray EvsController::webJson() const {
    QJsonObject o;
    o["source"] = live_.value("source").toString();
    o["running"] = live_.value("running").toBool();
    o["rate"] = live_.value("rate").toDouble();
    o["recText"] = live_.value("recText").toString();
    o["recColor"] = live_.value("recColor").toString();
    o["w"] = engine_ ? engine_->width() : 1280;
    o["h"] = engine_ ? engine_->height() : 720;
    const double fLo = std::max(0.0, d("specMin")), fHi = std::max(fLo + 1, d("specMax"));
    o["fmin"] = fLo;
    o["fmax"] = fHi;
    QJsonArray rois, spectra;
    const QVariantList rl = roiList();
    for (int i = 0; i < rl.size(); ++i) {
        QVariantMap m = rl[i].toMap();
        m.remove("mask");
        const RoiResult* r = i < (int)live_res_.size() ? &live_res_[i] : nullptr;
        m["f"] = r ? r->tone.f : 0.0;
        m["snr"] = r ? r->tone.snr : 0.0;
        m["ok"] = r ? r->ok : false;
        rois.append(QJsonObject::fromVariantMap(m));
        if (!r || r->f.empty()) continue;
        QJsonArray fa, aa;
        const size_t a = std::lower_bound(r->f.begin(), r->f.end(), fLo) - r->f.begin();
        const size_t b = std::min(r->amp.size(), (size_t)(std::upper_bound(r->f.begin(), r->f.end(), fHi) - r->f.begin()));
        const size_t step = std::max<size_t>(1, (b - a + 599) / 600);
        for (size_t k = a; k < b; k += step) {
            size_t best = k;
            for (size_t j = k; j < std::min(b, k + step); ++j) if (r->amp[j] > r->amp[best]) best = j;
            fa.append(r->f[best]);
            aa.append(r->amp[best]);
        }
        spectra.append(QJsonObject{{"name", rs_.rois[i].name}, {"color", rs_.rois[i].color.name()}, {"f", fa}, {"amp", aa}});
    }
    o["rois"] = rois;
    o["spectra"] = spectra;
    if (b("rpmOn")) o["rpm"] = QJsonObject{{"ok", live_rpm_.ok}, {"rpm", live_rpm_.ok ? live_rpm_.rpm : 0.0}, {"roi", live_.value("rpmRoi").toString()}};
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

MeasureSettings EvsController::liveSettings(bool am) const {
    MeasureSettings s;
    s.fs = kProbeFs;
    s.signal = am ? st_["amSignal"].toString() : st_["liveSignal"].toString();
    s.window = am ? st_["amWindow"].toString() : QString("hann");
    s.bandLo = d("bandLo");
    s.bandHi = d("bandHi");
    s.toneLo = d("toneLo");
    s.toneHi = d("toneHi");
    s.df = am ? 0.25 : 1.0;
    s.snrMin = am ? d("amSnr") : d("snrMin");
    s.fmax = std::max(d("specMax"), s.toneHi);
    s.commonMode = rs_.rois.size() >= 3;
    return s;
}

// the ROI's signal from its engine bins: ON-OFF (weighted by the thresholds), ON, OFF or all events; displacement = integrated
static std::vector<double> live_series(SignalKind k, double won, double woff, const ProbeTrace& tr, const MeasureSettings& s) {
    const size_t n = tr.on.size();
    std::vector<double> v(n);
    for (size_t j = 0; j < n; ++j) {
        switch (k) {
        case SignalKind::On: v[j] = tr.on[j]; break;
        case SignalKind::Off: v[j] = tr.off[j]; break;
        case SignalKind::Count: v[j] = tr.n[j]; break;
        default: v[j] = won * tr.on[j] - woff * tr.off[j]; break;
        }
    }
    if (s.signal != "displacement" || n < 64) return v;
    double acc = 0;
    for (double& x : v) { acc += x; x = acc; }
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (size_t j = 0; j < n; ++j) { sx += j; sy += v[j]; sxx += double(j) * j; sxy += j * v[j]; }
    const double den = n * sxx - sx * sx, kk = den != 0 ? (n * sxy - sx * sy) / den : 0, c0 = (sy - kk * sx) / n;
    for (size_t j = 0; j < n; ++j) v[j] -= c0 + kk * j;
    const auto sos = hz::dsp::butter(4, s.bandLo > 0 ? s.bandLo : 0, s.bandHi > 0 && s.bandHi < s.fs / 2 ? s.bandHi : 0, s.fs);
    return sos.empty() ? v : hz::dsp::sosfiltfilt(sos, v);
}

std::vector<double> EvsController::series(int i, const ProbeTrace& tr, const MeasureSettings& s) const {
    const SignalKind k = i >= 0 && i < (int)rs_.rois.size() ? rs_.rois[i].signal : SignalKind::Auto;
    return live_series(k, theta_on(d("bias_diff_on")), theta_off(d("bias_diff_off")), tr, s);
}

// ---- live analysis thread -----------------------------------------------------------------------------------------------------
void EvsController::pushLiveJob() {
    LiveJob j;
    j.s = liveSettings(false);
    j.win = d("liveWin");
    j.won = theta_on(d("bias_diff_on"));
    j.woff = theta_off(d("bias_diff_off"));
    for (const Roi& r : rs_.rois) j.kinds.push_back(r.signal);
    j.gen = roi_gen_;
    if (b("rpmOn") && !rs_.rois.empty()) {
        j.rpmIdx = std::max(0, rs_.indexOf(rs_.selected));
        j.rpm.marks = std::max(1, st_["rpmMarks"].toInt());
        j.rpm.rpmLo = std::max(1.0, d("rpmLo"));
        j.rpm.rpmHi = std::max(j.rpm.rpmLo + 1, d("rpmHi"));
        j.rpm.snrMin = d("snrMin");
        j.rpm.win = std::clamp(d("rpmWin"), 0.1, 10.0);
    }
    {
        std::lock_guard<std::mutex> lk(lw_mx_);
        lw_job_ = std::move(j);
    }
    lw_cv_.notify_all();
}

// newest worker results for the current ROI set; spectraNew = the spectra / tones were refreshed too
bool EvsController::pullLive(bool& spectraNew) {
    spectraNew = false;
    std::lock_guard<std::mutex> lk(lw_mx_);
    if (lw_seq_ == lw_seen_) return false;
    lw_seen_ = lw_seq_;
    if (lw_gen_out_ != roi_gen_ || lw_sig_.size() != rs_.rois.size()) return false;   // made for an older ROI set
    live_sig_ = std::move(lw_sig_);
    lw_sig_.clear();
    if (lw_spec_new_) {
        live_res_ = std::move(lw_res_);
        lw_res_.clear();
        lw_spec_new_ = false;
        if (lw_rpm_new_) {
            lw_rpm_new_ = false;
            live_rpm_ = lw_rpm_;
            if (!rpm_hist_.empty() && lw_rpm_t_ < rpm_hist_.back().first) rpm_hist_.clear();   // stream restarted
            rpm_hist_.push_back({lw_rpm_t_, live_rpm_.ok ? live_rpm_.rpm : NAN});
            while (rpm_hist_.size() > 1 && rpm_hist_.back().first - rpm_hist_.front().first > std::clamp(d("rpmHistS"), 5.0, 600.0)) rpm_hist_.pop_front();
            const int idx = std::max(0, rs_.indexOf(rs_.selected));
            live_["rpm"] = live_rpm_.ok ? live_rpm_.rpm : -1.0;
            live_["rpmOk"] = live_rpm_.ok;
            live_["rpmPass"] = live_rpm_.pass;
            live_["rpmSnr"] = live_rpm_.snr;
            live_["rpmNote"] = live_rpm_.note;
            live_["rpmRoi"] = idx < (int)rs_.rois.size() ? rs_.rois[idx].name : QString();
            live_["rpmColor"] = idx < (int)rs_.rois.size() ? rs_.rois[idx].color : QColor("#ff4fb0");
        }
        for (size_t i = 0; i < live_res_.size() && i < rs_.rois.size(); ++i) {
            live_res_[i].id = rs_.rois[i].id;
            live_res_[i].name = rs_.rois[i].name;
            live_res_[i].color = rs_.rois[i].color;
        }
        spectraNew = true;
    }
    return true;
}

// signals at the tick rate (30 Hz), spectra + tones every third pass
void EvsController::liveLoop() {
    int pass = 0;
    auto next = std::chrono::steady_clock::now();
    for (;;) {
        LiveJob job;
        {
            std::unique_lock<std::mutex> lk(lw_mx_);
            next += std::chrono::milliseconds(TICK_MS);
            const auto now = std::chrono::steady_clock::now();
            if (next < now) next = now;   // fell behind: don't try to catch up
            lw_cv_.wait_until(lk, next, [&] { return lw_quit_; });
            if (lw_quit_) return;
            job = lw_job_;
        }
        const bool spectra = pass++ % 3 == 0;
        hz::ProfScope prof_(spectra ? "live.analysis+fft" : "live.analysis");
        const int n = (int)job.kinds.size();
        const int keep = (int)(TRACE_S * kProbeFs);
        std::vector<std::vector<double>> sig(n);
        std::vector<RoiResult> res(spectra ? n : 0);
        for (int i = 0; i < n; ++i) {
            ProbeTrace tr;
            if (!engine_->probeWindow(i, job.win, tr, nullptr)) continue;
            std::vector<double> x = live_series(job.kinds[i], job.won, job.woff, tr, job.s);
            if (spectra) spectrum_of(x, job.s, res[i].f, res[i].amp);
            if ((int)x.size() > keep) x.erase(x.begin(), x.end() - keep);
            sig[i].swap(x);
        }
        if (spectra) pick_tones(res, job.s, nullptr);
        RpmResult rr;
        double rt = 0;
        bool rpmNew = false;
        if (spectra && job.rpmIdx >= 0 && job.rpmIdx < n) {
            ProbeTrace tr;
            int64_t tEnd = 0;
            if (engine_->probeWindow(job.rpmIdx, job.rpm.win, tr, &tEnd) && tr.on.size() >= 0.95 * job.rpm.win * kProbeFs) {   // full window only
                MeasureSettings vs = job.s;
                vs.signal = "velocity";
                rr = estimate_rpm(live_series(job.kinds[job.rpmIdx], job.won, job.woff, tr, vs), kProbeFs, job.rpm);
                rt = tEnd * 1e-6;
                rpmNew = true;
            }
        }
        std::lock_guard<std::mutex> lk(lw_mx_);
        if (rpmNew) { lw_rpm_ = rr; lw_rpm_t_ = rt; lw_rpm_new_ = true; }
        lw_sig_ = std::move(sig);
        if (spectra) { lw_res_ = std::move(res); lw_spec_new_ = true; }
        lw_gen_out_ = job.gen;
        ++lw_seq_;
    }
}

QVariantList EvsController::liveTones() const {
    QVariantList l;
    for (size_t i = 0; i < live_res_.size() && i < rs_.rois.size(); ++i) {
        const RoiResult& r = live_res_[i];
        l << QVariantMap{{"id", rs_.rois[i].id}, {"f", r.tone.f}, {"snr", r.tone.snr}, {"ok", r.ok}};
    }
    return l;
}

// ---- auto-measure: every trigger -> window [t + skip, + len] of each ROI from the engine ring -> tone -> log ----------------
void EvsController::processAutoMeasure() {
    std::vector<int64_t> nt;
    engine_->takeMeasureTriggers(nt);
    for (int64_t t : nt) {
        am_pending_.push_back(t);
        // keep the clip around the trigger: the ring holds the last second, so start "now" with the pre-trigger reaching back
        if (b("amSave") && rec_state_ == 0 && src_ && src_->running()) {
            const double now = live_.value("tNow").toDouble();
            const double pre = std::clamp(now - t * 1e-6 + 0.05, 0.05, 0.9);
            pending_desc_ = QString("auto-measure trigger at %1 s").arg(t * 1e-6, 0, 'f', 3);
            const QString path = recordingPath("am");
            if (engine_->record(path, rawHeader(), pre, std::max(0.05, d("amSaveLen")), false, d("trigFactor"))) {
                rec_state_ = 2;
                am_clip_ = QFileInfo(path).completeBaseName();
                am_clip_t_ = t;
            }
        }
    }
    if (am_pending_.empty()) return;
    const int64_t binT = engine_->binTime();
    const double skip = d("amSkipMs") * 1e-3, len = std::max(0.005, d("amLenMs") * 1e-3);
    const MeasureSettings s = liveSettings(true);
    std::vector<int64_t> still;
    for (int64_t t : am_pending_) {
        const int64_t a = t + (int64_t)(skip * 1e6), e = a + (int64_t)(len * 1e6);
        if (binT < e) { if (binT - t < 25'000'000) still.push_back(t); continue; }
        ++am_n_;
        std::vector<RoiResult> rr(rs_.rois.size());
        for (size_t i = 0; i < rs_.rois.size(); ++i) {
            rr[i].id = rs_.rois[i].id;
            rr[i].name = rs_.rois[i].name;
            rr[i].color = rs_.rois[i].color;
            ProbeTrace tr;
            if (!engine_->probeRange((int)i, a, e, tr)) continue;
            rr[i].sig = series((int)i, tr, s);
            for (float c : tr.n) rr[i].events += (long long)c;
            spectrum_of(rr[i].sig, s, rr[i].f, rr[i].amp);
        }
        pick_tones(rr, s, nullptr);
        // the ROI that rang hardest (largest tone amplitude among the accepted ones)
        int best = -1;
        for (int i = 0; i < (int)rr.size(); ++i) if (rr[i].ok && (best < 0 || rr[i].tone.amp > rr[best].tone.amp)) best = i;
        const QString clock = QTime::currentTime().toString("HH:mm:ss.zzz");
        QStringList others;
        QVariantList roisOut;
        for (int i = 0; i < (int)rr.size(); ++i) {
            roisOut << QVariantMap{{"name", rr[i].name}, {"f", rr[i].tone.f}, {"snr", rr[i].tone.snr}, {"ok", rr[i].ok}, {"color", rr[i].color}};
            if (i != best && rr[i].ok) others << QString("%1 %2").arg(rr[i].name).arg(rr[i].tone.f, 0, 'f', 1);
        }
        const QString clip = t == am_clip_t_ ? am_clip_ : QString();
        last_am_ = QVariantMap{{"n", am_n_}, {"time", clock}, {"t", t * 1e-6}, {"rois", roisOut}, {"ok", best >= 0},
                               {"best", best >= 0 ? rr[best].name : QString()}, {"f", best >= 0 ? rr[best].tone.f : 0.0},
                               {"snr", best >= 0 ? rr[best].tone.snr : 0.0}, {"color", best >= 0 ? rr[best].color : QColor("#6e7580")},
                               {"empty", rs_.rois.empty()}};
        std::vector<hz::Cell> row;
        row.push_back({QString::number(am_n_), QColor(), (double)am_n_});
        row.push_back({clock, QColor(), (double)am_n_});
        row.push_back({best >= 0 ? rr[best].name : QString("–"), best >= 0 ? rr[best].color : QColor(), (double)best});
        row.push_back({best >= 0 ? QString::number(rr[best].tone.f, 'f', 2) : QString("no tone"), best >= 0 ? QColor() : QColor("#ffc23d"),
                       best >= 0 ? rr[best].tone.f : 0});
        row.push_back({best >= 0 ? QString::number(rr[best].tone.snr, 'f', 0) : QString("–"), QColor(), best >= 0 ? rr[best].tone.snr : 0});
        row.push_back({others.join("  "), QColor(), 0, 0, false, true});
        row.push_back({clip, QColor(), 0, 0, false, true});
        am_rows_.insert(am_rows_.begin(), row);
        am_keys_.insert(am_keys_.begin(), am_n_);
        if (am_rows_.size() > 500) { am_rows_.pop_back(); am_keys_.pop_back(); }
        am_table_.set({"#", "Time", "ROI", "f (Hz)", "SNR", "Other ROIs", "Clip"}, am_rows_, am_keys_, {44, 96, 70, 80, 56, 220, 200});
        // log file in the project folder: one line per ROI
        if (!root_.isEmpty()) {
            const QString dir = QDir(root_).filePath(st_["project"].toString());
            QDir().mkpath(dir);
            const QString path = QDir(dir).filePath("automeasure_" + QDate::currentDate().toString("yyyyMMdd") + ".csv");
            QFile f(path);
            const bool fresh = !f.exists();
            if (f.open(QIODevice::Append | QIODevice::Text)) {
                QTextStream ts(&f);
                if (fresh) ts << "n,time,stream_t_s,roi,f_hz,snr,amp_rel,accepted,best,window_skip_ms,window_len_ms,signal,clip\n";
                for (int i = 0; i < (int)rr.size(); ++i)
                    ts << am_n_ << "," << clock << "," << QString::number(t * 1e-6, 'f', 4) << "," << rr[i].name << ","
                       << (rr[i].tone.f > 0 ? QString::number(rr[i].tone.f, 'f', 3) : QString()) << ","
                       << (rr[i].tone.f > 0 ? QString::number(rr[i].tone.snr, 'f', 1) : QString()) << ","
                       << (rr[i].tone.f > 0 ? QString::number(rr[i].tone.amp, 'g', 4) : QString()) << "," << (rr[i].ok ? 1 : 0) << ","
                       << (i == best ? 1 : 0) << "," << d("amSkipMs") << "," << d("amLenMs") << "," << s.signal << "," << clip << "\n";
            }
            am_csv_ = path;
        }
        emit amChanged();
    }
    am_pending_.swap(still);
}

void EvsController::clearAmLog() {
    am_rows_.clear();
    am_keys_.clear();
    am_n_ = 0;
    last_am_.clear();
    am_table_.set({"#", "Time", "ROI", "f (Hz)", "SNR", "Other ROIs", "Clip"}, {}, {}, {44, 96, 70, 80, 56, 220, 200});
    emit amChanged();
}

// ---- recording ----------------------------------------------------------------------------------------------------------
QString EvsController::recordingPath(const QString& tag) const {
    QString base = st_["recName"].toString().trimmed();
    base.replace(QRegularExpression("[^A-Za-z0-9_\\-]"), "_");
    if (base.isEmpty()) base = "rec";
    if (!tag.isEmpty()) base += "_" + tag;
    const QString name = base + "_" + QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    const QString dir = QDir(root_.isEmpty() ? QDir::homePath() : root_).filePath(st_["project"].toString());
    QDir().mkpath(dir);
    QString p = QDir(dir).filePath(name + ".raw");
    for (int k = 2; QFileInfo::exists(p); ++k) p = QDir(dir).filePath(QString("%1_%2.raw").arg(name).arg(k));
    return p;
}

std::string EvsController::rawHeader() const {
    return raw_header(engine_->width(), engine_->height(),
                      {{"camera_integrator_name", "Lucid"}, {"recorder", "hertzyevs"}, {"source", src_ ? src_->description().toStdString() : std::string()}});
}

void EvsController::recordNow() {
    if (!src_ || !src_->running()) { emit toast("Start the camera first"); return; }
    if (rec_state_ == 2 || rec_state_ == 3) return;   // (not recStatus(): that would consume the "finished" flag tick() needs)
    if (rec_state_ == 1) engine_->cancel();
    pending_desc_.clear();
    if (!engine_->record(recordingPath(), rawHeader(), d("preMs") / 1000.0, d("recLen"), false, d("trigFactor")))
        emit toast("Could not start the recording");
    else rec_state_ = 2;
}

void EvsController::toggleArm() {
    if (!src_ || !src_->running()) { emit toast("Start the camera first"); return; }
    if (rec_state_ == 1) { engine_->cancel(); rec_state_ = 0; return; }
    if (rec_state_ != 0) return;
    pending_desc_.clear();
    if (engine_->record(recordingPath(), rawHeader(), d("preMs") / 1000.0, d("recLen"), true, d("trigFactor"))) rec_state_ = 1;
}

void EvsController::cancelRecording() { engine_->cancel(); }
void EvsController::simTap() { if (src_) src_->tap(); }

void EvsController::finishRecording(const RecStatus& rs) {
    if (rs.tTrigger < 0) return;
    writeSidecar(rs, rs.path);
    last_rec_ = rs.path;
    if (!pending_desc_.startsWith("auto-measure"))
        emit toast("Saved " + QFileInfo(rs.path).fileName() + QString("  (%1 MB, %2 M events)").arg(rs.bytes / 1048576.0, 0, 'f', 1).arg(rs.events / 1e6, 0, 'f', 2));
    emit recordingSaved(rs.path);
}

void EvsController::writeSidecar(const RecStatus& rs, const QString& raw) {
    QJsonObject o;
    o["raw"] = QFileInfo(raw).fileName();
    o["created"] = QDateTime::currentDateTime().toString(Qt::ISODate);
    o["recorder"] = "hertzyevs";
    o["recorder_version"] = 2;
    o["source"] = src_ ? src_->description() : QString();
    o["project"] = st_["project"].toString();
    QString desc = st_["recDesc"].toString().trimmed();
    if (!pending_desc_.isEmpty()) desc = desc.isEmpty() ? pending_desc_ : desc + " (" + pending_desc_ + ")";
    o["description"] = desc;
    o["t_trigger_us"] = (double)rs.tTrigger;
    o["t_start_us"] = (double)rs.tStart;
    o["t_end_us"] = (double)rs.tEnd;
    o["t_first_us"] = (double)rs.tFirst;
    o["trigger_s_in_file"] = rs.tFirst >= 0 ? (rs.tTrigger - rs.tFirst) * 1e-6 : d("preMs") / 1000.0;
    o["pre_s"] = (rs.tTrigger - rs.tStart) * 1e-6;
    o["len_s"] = (rs.tEnd - rs.tTrigger) * 1e-6;
    o["events"] = (double)rs.events;
    o["bytes"] = (double)rs.bytes;
    o["edge_contrast"] = d("contrast");
    QJsonObject bias;
    for (const char* k : {"bias_diff_on", "bias_diff_off", "bias_fo", "bias_hpf", "bias_refr"}) bias[k] = d(k);
    o["biases"] = bias;
    o["preset"] = st_["preset"].toString();
    o["hardware_filters"] = QJsonObject{{"afk_enable", b("afk_enable")}, {"afk_band_stop_hz", QJsonArray{d("afk_low"), d("afk_high")}},
                                        {"erc_enable", b("erc_enable")}, {"erc_rate_mev_s", d("erc_rate")}, {"trail_enable", b("trail_enable")},
                                        {"roi_enable", b("roiOn") && b("roiHw")},
                                        {"roi", QJsonArray{d("roiX"), d("roiY"), d("roiW"), d("roiH")}}};
    QJsonObject nodes;
    if (src_) for (const QString& k : kFeatureKeys) nodes[k] = src_->nodeFor(k);
    o["camera_nodes"] = nodes;
    o["live_rois"] = rs_.toJson();
    o["live_settings"] = QJsonObject{{"signal", st_["liveSignal"].toString()}, {"band_hz", QJsonArray{d("bandLo"), d("bandHi")}},
                                     {"tone_hz", QJsonArray{d("toneLo"), d("toneHi")}}};
    QFile f(raw.left(raw.size() - 4) + ".json");
    if (f.open(QIODevice::WriteOnly)) f.write(QJsonDocument(o).toJson());
}

// ---- camera page ----------------------------------------------------------------------------------------------------------
QVariantList EvsController::nodeList(const QString& filter) {
    QVariantList l;
    if (!src_) return l;
    for (const NodeInfo& n : src_->nodes(filter))
        l << QVariantMap{{"name", n.name}, {"display", n.display}, {"type", n.type}, {"value", n.value}, {"tip", n.tip},
                         {"writable", n.writable}, {"min", n.min}, {"max", n.max}, {"entries", n.entries}};
    return l;
}

bool EvsController::setNode(const QString& name, const QString& value) {
    if (!src_) return false;
    QString err;
    const bool ok = src_->setNode(name, value, &err);
    if (!ok) emit toast(err);
    emit featuresChanged();
    return ok;
}

void EvsController::setNodeOverride(const QString& key, const QString& node) {
    if (!src_) return;
    src_->setNodeOverride(key, node);
    QSettings().setValue("evs/override/" + key, node);
    reapplyAll();
}

void EvsController::downloadXml() {
    if (!src_) return;
    const QString dir = QDir(root_.isEmpty() ? QDir::homePath() : root_).filePath("camera_xml");
    src_->downloadXml(dir);
    emit toast("Camera XML saved in " + dir);
}

// ---- plots ------------------------------------------------------------------------------------------------------------------
void EvsController::fillPlot(const QString& kind, hz::PlotData& pd) {
    if (kind == "live:trace") fillTrace(pd);
    else if (kind == "live:spectrum") fillSpectrum(pd);
    else if (kind == "live:rate") fillRate(pd);
    else if (kind == "live:rpm") fillRpm(pd);
}

void EvsController::fillTrace(hz::PlotData& pd) {
    const bool disp = st_["liveSignal"] == "displacement";
    pd.xlabel = "time (ms)";
    pd.ylabel = disp ? QString("integrated, %1–%2 Hz (rel.)").arg(d("bandLo"), 0, 'f', 0).arg(d("bandHi"), 0, 'f', 0) : QString("ON−OFF per 0.1 ms (rel.)");
    pd.xunit = "ms";
    pd.empty = rs_.rois.empty() ? "Draw a ROI on the view (line, rectangle, polygon or brush)" : "Waiting for events";
    pd.fixX = true;
    pd.x0 = -TRACE_S * 1000;
    pd.x1 = 0;
    double ymax = 0;
    for (int i = 0; i < (int)live_sig_.size() && i < (int)rs_.rois.size(); ++i) {
        const auto& x = live_sig_[i];
        if (x.empty()) continue;
        hz::PlotSeries s{hz::PlotSeries::Line, rs_.rois[i].name, rs_.rois[i].color};
        s.width = rs_.rois[i].id == rs_.selected ? 2.0f : 1.1f;
        const int n = (int)x.size(), step = std::max(1, n / 1500);
        for (int k = 0; k < n; k += step) {
            s.x.push_back((k - n) / kProbeFs * 1000);
            s.y.push_back(x[k]);
            ymax = std::max(ymax, std::fabs(x[k]));
        }
        pd.series.push_back(std::move(s));
    }
    pd.fixY = true;
    const double y = std::max(1e-6, ymax * 1.15);
    pd.y0 = -y;
    pd.y1 = y;
}

void EvsController::fillSpectrum(hz::PlotData& pd) {
    pd.xlabel = "frequency (Hz)";
    pd.ylabel = "amplitude (rel.)";
    pd.xunit = "Hz";
    pd.empty = rs_.rois.empty() ? "Draw a ROI to see its spectrum" : "Waiting for events";
    // plotted range [specMin, specMax]: only those bins go to the plot, so the y scale follows the peaks inside it
    const double fLo = std::max(0.0, d("specMin")), fHi = std::max(fLo + 1, d("specMax"));
    pd.fixX = true;
    pd.x0 = fLo;
    pd.x1 = fHi;
    pd.yFromZero = true;
    for (int i = 0; i < (int)live_res_.size() && i < (int)rs_.rois.size(); ++i) {
        const RoiResult& o = live_res_[i];
        if (o.f.empty()) continue;
        hz::PlotSeries s{hz::PlotSeries::Line, rs_.rois[i].name, rs_.rois[i].color};
        s.width = rs_.rois[i].id == rs_.selected ? 2.0f : 1.3f;
        const size_t a = std::lower_bound(o.f.begin(), o.f.end(), fLo) - o.f.begin();
        const size_t b = std::upper_bound(o.f.begin(), o.f.end(), fHi) - o.f.begin();
        const size_t a0 = a > 0 ? a - 1 : 0, b1 = std::min(o.f.size(), b + 1);
        s.x.assign(o.f.begin() + a0, o.f.begin() + b1);
        s.y.assign(o.amp.begin() + a0, o.amp.begin() + std::min(o.amp.size(), b1));
        pd.series.push_back(std::move(s));
        if (o.ok && o.tone.f >= fLo && o.tone.f <= fHi) pd.texts.push_back({o.tone.f, o.tone.amp, QString("%1 Hz").arg(o.tone.f, 0, 'f', 1), rs_.rois[i].color});
    }
    for (double f : {d("toneLo"), d("toneHi")}) {
        if (f <= fLo || f >= fHi) continue;
        hz::PlotMark m;
        m.v = f;
        m.color = QColor(76, 155, 255, 150);
        m.dashed = true;
        pd.marks.push_back(m);
    }
}

void EvsController::fillRpm(hz::PlotData& pd) {
    pd.xlabel = "time (s)";
    pd.ylabel = "speed (rpm)";
    pd.xunit = "s";
    pd.yunit = "rpm";
    pd.legend = false;
    pd.empty = !b("rpmOn") ? "Switch RPM on (side panel)" : rs_.rois.empty() ? "Draw a ROI where the rotating part passes" : "Waiting for a speed";
    if (!b("rpmOn") || rpm_hist_.empty()) return;
    const double tNow = rpm_hist_.back().first, span = std::clamp(d("rpmHistS"), 5.0, 600.0);
    pd.fixX = true;
    pd.x0 = -span;
    pd.x1 = 0;
    const int idx = std::max(0, rs_.indexOf(rs_.selected));
    const QColor col = idx < (int)rs_.rois.size() ? rs_.rois[idx].color : QColor("#ff4fb0");
    hz::PlotSeries s{hz::PlotSeries::Line, "rpm", col};
    s.width = 1.6f;
    for (const auto& h : rpm_hist_) {
        if (!std::isfinite(h.second)) continue;
        s.x.push_back(h.first - tNow);
        s.y.push_back(h.second);
    }
    if (s.x.empty()) return;
    const double lo = *std::min_element(s.y.begin(), s.y.end()), hi = *std::max_element(s.y.begin(), s.y.end());
    const double pad = std::max(1.0, 0.1 * (hi - lo) + 0.002 * hi);
    pd.fixY = true;
    pd.y0 = lo - pad;
    pd.y1 = hi + pad;
    pd.series.push_back(std::move(s));
}

void EvsController::fillRate(hz::PlotData& pd) {
    pd.xlabel = "time (s)";
    pd.ylabel = "events (Mev/s)";
    pd.xunit = "s";
    pd.yunit = "Mev/s";
    pd.empty = "No events";
    pd.fixX = true;
    pd.x0 = -5;
    pd.x1 = 0;
    pd.yFromZero = true;
    const int n = (int)rate_all_.size();
    if (n == 0) return;
    const int step = std::max(1, n / 1000);
    hz::PlotSeries a{hz::PlotSeries::Line, "all", QColor(150, 158, 170)}, r{hz::PlotSeries::Line, b("roiOn") ? "crop" : "ROIs", QColor("#ff4fb0")};
    a.width = 1;
    r.width = 1.4f;
    for (int k = 0; k + step <= n; k += step) {
        double sa = 0, sr = 0;
        for (int j = k; j < k + step; ++j) { sa += rate_all_[j]; sr += rate_roi_[j]; }
        const double t = (k + step / 2.0 - n) / 1000.0;
        a.x.push_back(t); a.y.push_back(sa / step / 1e6);
        r.x.push_back(t); r.y.push_back(sr / step / 1e6);
    }
    pd.series.push_back(std::move(a));
    pd.series.push_back(std::move(r));
}

}  // namespace evs

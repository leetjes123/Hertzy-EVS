#include "player.hpp"
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QPainter>
#include <QPainterPath>
#include <QProcess>
#include <QSettings>
#include <QCoreApplication>
#include <QStandardPaths>
#include <QThread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <algorithm>
#include <cmath>
#include "roictl.hpp"
#include "prof.hpp"

namespace evs {

static const QRgb kBg = qRgb(10, 11, 14);

// sleep about `us` microseconds: Windows' Sleep / timed waits are 15.6 ms coarse, a high-resolution waitable timer is not
static void nap_us(int us) {
#ifdef _WIN32
    thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /*CREATE_WAITABLE_TIMER_HIGH_RESOLUTION*/, TIMER_ALL_ACCESS);
    if (timer) {
        LARGE_INTEGER due;
        due.QuadPart = -10LL * us;   // relative, 100 ns units
        if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) { WaitForSingleObject(timer, INFINITE); return; }
    }
#endif
    QThread::usleep(us);
}

// split [0, n) over a few threads (pictures are ~0.9 M pixels)
template <class F>
static void par(size_t n, F&& f) {
    static const unsigned nth = std::clamp(std::thread::hardware_concurrency() / 2, 1u, 4u);
    if (nth <= 1 || n < 100000) { f(0, n); return; }
    std::thread th[4];
    const size_t step = (n + nth - 1) / nth;
    for (unsigned k = 1; k < nth; ++k) th[k] = std::thread([&, k] { f(std::min(n, k * step), std::min(n, (k + 1) * step)); });
    f(0, std::min(n, step));
    for (unsigned k = 1; k < nth; ++k) th[k].join();
}

static QRgb mix(QRgb a, QRgb b, float w) {   // w = weight of b
    return qRgb(int(qRed(a) + (qRed(b) - qRed(a)) * w), int(qGreen(a) + (qGreen(b) - qGreen(a)) * w), int(qBlue(a) + (qBlue(b) - qBlue(a)) * w));
}

// ---- frame renderer ------------------------------------------------------------------------------------------------------------
void FrameRenderer::ensureSurface(const Clip& c, double t, double span) {
    const size_t n = (size_t)c.W * c.H;
    if (ts_.size() != n || W_ != c.W || H_ != c.H) { ts_.assign(n, 0); tp_.assign(n, 0); W_ = c.W; H_ = c.H; last_ = -1; }
    size_t a, b = c.at(t);
    if (last_ >= 0 && t >= last_ && t - last_ <= span && span <= span_ + 1e-12) {
        a = c.at(last_);   // forward by less than the span: only the new events
    } else {
        std::fill(ts_.begin(), ts_.end(), 0u);
        a = c.at(std::max(0.0, t - span));
    }
    for (size_t i = a; i < b; ++i) {
        const uint32_t v = c.xyp[i];
        const size_t idx = (size_t)Clip::ey(v) * c.W + Clip::ex(v);
        if (idx >= n) continue;
        ts_[idx] = c.t[i] + 1;
        tp_[idx] = Clip::ep(v);
    }
    last_ = t;
    span_ = span;
}

double FrameRenderer::timeAt(const Clip& c, double x, double y, int r) const {
    if (ts_.empty()) return -1;
    const int cx = (int)std::floor(x), cy = (int)std::floor(y);
    uint32_t best = 0;
    for (int yy = std::max(0, cy - r); yy <= std::min(c.H - 1, cy + r); ++yy)
        for (int xx = std::max(0, cx - r); xx <= std::min(c.W - 1, cx + r); ++xx) best = std::max(best, ts_[(size_t)yy * c.W + xx]);
    return best ? (best - 1) * 1e-6 : -1;
}

QImage FrameRenderer::render(const Clip& c, const PlayParams& p, double t) {
    hz::ProfScope prof_("player.render");
    QImage img(c.W, c.H, QImage::Format_RGB32);
    auto* dst = reinterpret_cast<QRgb*>(img.bits());
    const size_t n = (size_t)c.W * c.H;
    const int64_t tus = (int64_t)std::llround(t * 1e6);
    if (p.mode == "trail") {
        const double span = std::max(0.01, p.trailMs) * 1e-3;
        ensureSurface(c, t, span);
        const double spanUs = span * 1e6;
        // newest = white-yellow, older = orange -> purple -> deep blue, fading out
        static QRgb lut[256];
        static bool init = false;
        if (!init) {
            for (int k = 0; k < 256; ++k) {
                const float x = k / 255.f;   // 0 = newest
                const QRgb col = x < 0.04f ? mix(qRgb(255, 255, 255), turbo(0.93f), x / 0.04f) : turbo(0.93f - 0.85f * x);
                lut[k] = mix(kBg, col, std::pow(1.f - x, 0.55f));
            }
            init = true;
        }
        const uint32_t* ts = ts_.data();
        par(n, [&](size_t a, size_t b) {
            for (size_t i = a; i < b; ++i) {
                const uint32_t v = ts[i];
                if (!v) { dst[i] = kBg; continue; }
                const int64_t age = tus - (int64_t)(v - 1);
                if (age < 0 || age > spanUs) { dst[i] = kBg; continue; }
                dst[i] = lut[std::min(255, (int)(age / spanUs * 255))];
            }
        });
        return img;
    }
    const double exp = std::max(1.0, p.exposureUs) * 1e-6;
    const size_t a = c.at(t - exp), b = c.at(t);
    if (p.mode == "heat") {
        cnt_.assign(n, 0.f);
        float mx = 1.f;
        for (size_t i = a; i < b; ++i) {
            const uint32_t v = c.xyp[i];
            const size_t idx = (size_t)Clip::ey(v) * c.W + Clip::ex(v);
            if (idx >= n) continue;
            mx = std::max(mx, cnt_[idx] += 1.f);
            if (p.bold) {
                if (Clip::ex(v) + 1 < c.W) cnt_[idx + 1] += 1.f;
                if (Clip::ey(v) + 1 < c.H) cnt_[idx + c.W] += 1.f;
            }
        }
        const float inv = 1.f / std::log1p(mx);
        const float* cn = cnt_.data();
        par(n, [&](size_t lo, size_t hi) {
            for (size_t i = lo; i < hi; ++i) dst[i] = cn[i] > 0 ? inferno(0.15f + 0.85f * std::log1p(cn[i]) * inv) : kBg;
        });
        return img;
    }
    // events: ON blue-white, OFF orange; newer = brighter
    static QRgb lutOn[64], lutOff[64];
    static bool init = false;
    if (!init) {
        for (int k = 0; k < 64; ++k) {
            const float w = 1.f - 0.5f * k / 63.f;
            lutOn[k] = mix(kBg, qRgb(120, 200, 255), w);
            lutOff[k] = mix(kBg, qRgb(255, 140, 70), w);
        }
        init = true;
    }
    par(n, [&](size_t lo, size_t hi) { std::fill(dst + lo, dst + hi, kBg); });
    const double expUs = exp * 1e6;
    for (size_t i = a; i < b; ++i) {
        const uint32_t v = c.xyp[i];
        const size_t idx = (size_t)Clip::ey(v) * c.W + Clip::ex(v);
        if (idx >= n) continue;
        const int k = std::clamp((int)((tus - (int64_t)c.t[i]) / expUs * 63), 0, 63);
        const QRgb col = Clip::ep(v) ? lutOn[k] : lutOff[k];
        dst[idx] = col;
        if (p.bold) {
            const bool rx = Clip::ex(v) + 1 < c.W, ry = Clip::ey(v) + 1 < c.H;
            if (rx) dst[idx + 1] = col;
            if (ry) dst[idx + c.W] = col;
            if (rx && ry) dst[idx + c.W + 1] = col;
        }
    }
    return img;
}

// ---- player -------------------------------------------------------------------------------------------------------------------
Player::Player(RoiController* rois, QObject* parent) : hz::PlotSource(parent), roiCtl_(rois) {
    cfg_ = QVariantMap{{"active", false}, {"mode", "events"}, {"speed", 0.01}, {"exposureUs", 200.0}, {"trailMs", 5.0},
                       {"strobe", false}, {"strobeHz", 50.0}, {"strobeDrift", 1.0}, {"loop", "window"}, {"plotWin", 0.2}, {"bold", true}};
    QSettings s;
    const QVariantMap saved = s.value("evs/player").toMap();
    for (auto it = saved.begin(); it != saved.end(); ++it) if (cfg_.contains(it.key()) && it.key() != "active") cfg_[it.key()] = it.value();
    p_ = params();
    plotWin_ = cfg_["plotWin"].toDouble();
    loopWindow_ = cfg_["loop"] == "window";
    exp_ = QVariantMap{{"busy", false}, {"progress", 0.0}, {"text", ""}};
    connect(&gui_, &QTimer::timeout, this, &Player::pollGui);
    gui_.start(33);
    th_ = std::thread([this] { loop(); });
    anTh_ = std::thread([this] { anaLoop(); });
}

Player::~Player() {
    exCancel_ = true;
    if (exTh_.joinable()) exTh_.join();
    {
        std::lock_guard<std::mutex> lk(mx_);
        quit_ = true;
    }
    cv_.notify_all();
    anCv_.notify_all();
    if (th_.joinable()) th_.join();
    if (anTh_.joinable()) anTh_.join();
}

PlayParams Player::params() const {
    PlayParams p;
    p.mode = cfg_["mode"].toString();
    p.speed = std::clamp(cfg_["speed"].toDouble(), 1e-6, 10.0);
    p.exposureUs = std::clamp(cfg_["exposureUs"].toDouble(), 1.0, 1e6);
    p.trailMs = std::clamp(cfg_["trailMs"].toDouble(), 0.01, 5000.0);
    p.strobe = cfg_["strobe"].toBool();
    p.strobeHz = std::clamp(cfg_["strobeHz"].toDouble(), 0.01, 1e5);
    p.strobeDrift = std::clamp(cfg_["strobeDrift"].toDouble(), -50.0, 50.0);
    p.fps = p.strobe ? 30 : 60;
    p.bold = cfg_["bold"].toBool();
    return p;
}

void Player::range(double& a, double& b) const {
    const double dur = clip_ ? clip_->duration : 0;
    if (loopWindow_ && winOk_ && winB_ > winA_) { a = std::clamp(winA_, 0.0, dur); b = std::clamp(winB_, 0.0, dur); }
    else { a = 0; b = dur; }
    if (b <= a) { a = 0; b = dur; }
}

void Player::setClip(std::shared_ptr<const Clip> c) {
    if (c == clipGui_) return;   // (clipChanged also fires for info updates of the same clip)
    {
        std::lock_guard<std::mutex> lk(mx_);
        clip_ = c;
        dirty_ = plotDirty_ = true;
        playing_ = false;
        double a, b;
        range(a, b);
        t_ = c ? std::min(c->duration, a + 0.02) : 0.0;
    }
    clipGui_ = c;
    probe_.reset();
    {
        std::lock_guard<std::mutex> lk(fmx_);
        frame_ = QImage();
        res_.clear();
        sig_.clear();
        ++seq_;
    }
    resGui_.clear();
    sigGui_.clear();
    cv_.notify_all();
    emit clipChanged();
    emit posChanged();
    emit plotsChanged();
}

void Player::setRois(const std::vector<Roi>& rois, const MeasureSettings& s) {
    {
        std::lock_guard<std::mutex> lk(mx_);
        rois_ = rois;
        ms_ = s;
        ++roiGen_;
        plotDirty_ = true;
    }
    cv_.notify_all();
}

void Player::setWindow(double t0, double t1, bool ok) {
    std::lock_guard<std::mutex> lk(mx_);
    winA_ = t0;
    winB_ = t1;
    winOk_ = ok;
}

void Player::setv(const QString& key, const QVariant& v) {
    if (!cfg_.contains(key) || cfg_[key] == v) return;
    cfg_[key] = v;
    {
        std::lock_guard<std::mutex> lk(mx_);
        p_ = params();
        plotWin_ = std::clamp(cfg_["plotWin"].toDouble(), 0.01, 5.0);
        loopWindow_ = cfg_["loop"] == "window";
        dirty_ = true;
        if (key == "plotWin") plotDirty_ = true;
    }
    cv_.notify_all();
    QVariantMap keep = cfg_;
    keep.remove("active");
    QSettings().setValue("evs/player", keep);
    if (key == "active" && !v.toBool()) pause();
    emit cfgChanged();
}

void Player::play() {
    if (!clipGui_) return;
    {
        std::lock_guard<std::mutex> lk(mx_);
        double a, b;
        range(a, b);
        if (t_ >= b - 1e-6 || t_ < a) t_ = a + std::min(0.02, (b - a) / 2);   // at the end: start over
        playing_ = true;
        dirty_ = true;
    }
    cv_.notify_all();
    emit posChanged();
}

void Player::pause() {
    playing_ = false;
    emit posChanged();
}

void Player::toggle() { if (playing_) pause(); else play(); }

void Player::seek(double s) {
    if (!clipGui_) return;
    {
        std::lock_guard<std::mutex> lk(mx_);
        t_ = std::clamp(s, 0.0, clipGui_->duration);
        dirty_ = plotDirty_ = true;
    }
    cv_.notify_all();
    emit posChanged();
}

void Player::step(int n) {
    {
        std::lock_guard<std::mutex> lk(mx_);
        stepReq_ += n;
    }
    playing_ = false;
    cv_.notify_all();
}

void Player::home() {
    std::lock_guard<std::mutex> lk(mx_);
    double a, b;
    range(a, b);
    t_ = a + std::min(0.002, (b - a) / 2);
    dirty_ = plotDirty_ = true;
    cv_.notify_all();
}

QVariantMap Player::timeAt(double x, double y) {
    QVariantMap m{{"ok", false}};
    if (!clipGui_) return m;
    const PlayParams p = params();
    const double span = std::max(p.mode == "trail" ? p.trailMs * 1e-3 : 0.0, std::max(p.exposureUs * 1e-6, 0.002));
    probe_.ensureSurface(*clipGui_, t_.load(), span);
    const double ts = probe_.timeAt(*clipGui_, x, y, 2);
    if (ts < 0) return m;
    m["ok"] = true;
    m["t"] = ts;
    m["age"] = t_.load() - ts;
    return m;
}

bool Player::takeFrame(QImage& out, uint64_t& seq) {
    std::lock_guard<std::mutex> lk(fmx_);
    if (seq == seq_ || frame_.isNull()) return false;
    out = frame_;
    seq = seq_;
    return true;
}

bool Player::takeOverlay(QImage& out, uint64_t& seq) { return roiCtl_ && roiCtl_->takeOverlay(out, seq); }

// ---- player thread -------------------------------------------------------------------------------------------------------------
void Player::loop() {
    FrameRenderer r;
    std::shared_ptr<const Clip> lastClip;
    auto lastWall = std::chrono::steady_clock::now(), lastPlot = lastWall - std::chrono::seconds(1);
    double plotT = -1;
    for (;;) {
        std::shared_ptr<const Clip> c;
        PlayParams p;
        double a = 0, b = 0, win = 0.2;
        int steps = 0;
        bool force, plotForce;
        {
            std::unique_lock<std::mutex> lk(mx_);
            auto pred = [&] { return quit_ || dirty_ || stepReq_ != 0 || plotDirty_; };
            if (playing_) {
                // pace the pictures with 1 ms naps: a timed condition wait is only 15.6 ms accurate on Windows
                const auto deadline = lastWall + std::chrono::microseconds((int64_t)(1e6 / p_.fps));
                while (!pred() && std::chrono::steady_clock::now() < deadline) {
                    lk.unlock();
                    nap_us(1000);
                    lk.lock();
                }
            } else cv_.wait_for(lk, std::chrono::milliseconds(200), pred);
            if (quit_) return;
            c = clip_;
            p = p_;
            range(a, b);
            win = plotWin_;
            steps = stepReq_;
            stepReq_ = 0;
            force = dirty_;
            dirty_ = false;
            plotForce = plotDirty_;
            plotDirty_ = false;
        }
        const auto now = std::chrono::steady_clock::now();
        const double dt = std::min(0.1, std::chrono::duration<double>(now - lastWall).count());
        lastWall = now;
        if (!c) continue;
        if (c != lastClip) { r.reset(); lastClip = c; force = true; }
        double t;
        bool moved;
        {
            // advance under the lock so a seek from the GUI is never overwritten by a stale position
            std::lock_guard<std::mutex> lk(mx_);
            t = t_.load();
            const double sstep = (1.0 / p.strobeHz) * (1 + p.strobeDrift / 100);
            if (playing_) t += p.strobe ? sstep : p.speed * dt;
            if (steps) t += steps * (p.strobe ? sstep : p.exposureUs * 1e-6 * (p.mode == "trail" ? 4 : 1));
            if (playing_ && t > b) t = a + std::min(p.strobe ? 0.0 : p.exposureUs * 1e-6, (b - a) / 2);   // loop
            t = std::clamp(t, 0.0, c->duration);
            moved = t != t_.load();
            t_ = t;
        }
        if (moved || force || steps) {
            QImage img = r.render(*c, p, t);
            std::lock_guard<std::mutex> lk(fmx_);
            frame_ = std::move(img);
            ++seq_;
        }
        // plots at the playhead: hand the position to the analysis thread (it takes the newest one)
        if ((plotForce || std::fabs(t - plotT) > 1e-9) && now - lastPlot >= std::chrono::milliseconds(100)) {
            lastPlot = now;
            plotT = t;
            {
                std::lock_guard<std::mutex> lk(mx_);
                anReqT_ = t;
            }
            anCv_.notify_all();
        }
    }
}

// ROI signals of the window before the playhead, all ROIs in one pass over the events (pixel -> ROI look-up), then
// spectra + tones. Signal per ROI as in the analysis: sided ON-OFF for lines, ON-OFF, ON, OFF or all events.
void Player::anaLoop() {
    std::vector<uint16_t> lut;
    std::vector<int8_t> sign;
    uint64_t lutGen = ~0ull;
    const Clip* lutClip = nullptr;
    for (;;) {
        std::shared_ptr<const Clip> c;
        std::vector<Roi> rois;
        MeasureSettings ms;
        double t, win;
        uint64_t gen;
        {
            std::unique_lock<std::mutex> lk(mx_);
            anCv_.wait(lk, [&] { return quit_ || anReqT_ >= 0; });
            if (quit_) return;
            t = anReqT_;
            anReqT_ = -1;
            c = clip_;
            rois = rois_;
            ms = ms_;
            win = plotWin_;
            gen = roiGen_;
        }
        if (!c) continue;
        hz::ProfScope prof_("player.analysis");
        if (gen != lutGen || c.get() != lutClip) {
            build_lut(rois, c->W, c->H, lut, sign);
            lutGen = gen;
            lutClip = c.get();
        }
        const int nr = (int)rois.size();
        ms.df = std::max(ms.df, 1.0);   // live view: 1 Hz bins are plenty (and 4x less FFT)
        const double fs = ms.fs, ta = std::max(0.0, t - win);
        const int nb = std::max(0, (int)std::floor((t - ta) * fs + 1e-9));
        std::vector<std::vector<double>> on(nr, std::vector<double>(nb, 0.0)), off = on, onU = on, offU = on;
        const size_t a = c->at(ta), b = c->at(t);
        const double t0us = ta * 1e6, k = fs * 1e-6;
        const size_t npx = lut.size();
        for (size_t e = a; e < b; ++e) {
            const uint32_t v = c->xyp[e];
            const size_t idx = (size_t)Clip::ey(v) * c->W + Clip::ex(v);
            if (idx >= npx) continue;
            const int r = lut[idx];
            if (!r || r > nr) continue;
            const int bin = (int)((c->t[e] - t0us) * k);
            if (bin < 0 || bin >= nb) continue;
            const double sg = sign[idx] ? sign[idx] : 1;
            if (Clip::ep(v)) { on[r - 1][bin] += sg; onU[r - 1][bin] += 1; }
            else { off[r - 1][bin] += sg; offU[r - 1][bin] += 1; }
        }
        std::vector<RoiResult> res(nr);
        std::vector<std::vector<double>> sig(nr);
        for (int i = 0; i < nr; ++i) {
            const Roi& roi = rois[i];
            res[i].id = roi.id;
            res[i].name = roi.name;
            res[i].color = roi.color;
            std::vector<double>& x = sig[i];
            x.resize(nb);
            for (int j = 0; j < nb; ++j) {
                switch (roi.signal) {
                case SignalKind::On: x[j] = onU[i][j]; break;
                case SignalKind::Off: x[j] = offU[i][j]; break;
                case SignalKind::Count: x[j] = onU[i][j] + offU[i][j]; break;
                default: x[j] = c->thetaOn * on[i][j] - c->thetaOff * off[i][j]; break;
                }
            }
            spectrum_of(x, ms, res[i].f, res[i].amp);
        }
        pick_tones(res, ms, nullptr);
        std::lock_guard<std::mutex> lk(fmx_);
        res_ = std::move(res);
        sig_ = std::move(sig);
        resT_ = t;
        ++resSeq_;
    }
}

void Player::pollGui() {
    const double t = t_.load();
    const bool pl = playing_.load();
    if (t != lastPosT_ || pl != lastPlaying_) { lastPosT_ = t; lastPlaying_ = pl; emit posChanged(); }
    bool fresh = false;
    {
        std::lock_guard<std::mutex> lk(fmx_);
        if (resSeq_ != resSeen_) {
            resSeen_ = resSeq_;
            resGui_ = res_;
            sigGui_ = sig_;
            resTGui_ = resT_;
            fresh = true;
        }
    }
    if (fresh) { emit tonesChanged(); emit plotsChanged(); }
    // export progress
    bool done = false;
    QString msg;
    {
        std::lock_guard<std::mutex> lk(exmx_);
        if (exDone_) { done = true; msg = exMsg_; exDone_ = false; }
    }
    const bool busy = exBusy_.load();
    if (busy != exp_["busy"].toBool() || (busy && std::fabs(exProgress_.load() - exp_["progress"].toDouble()) > 0.004) || done) {
        exp_["busy"] = busy;
        exp_["progress"] = exProgress_.load();
        if (done) exp_["text"] = msg;
        emit exportChanged();
    }
    if (done) {
        if (exTh_.joinable()) exTh_.join();
        emit toast(msg);
    }
}

QVariantList Player::tones() const {
    QVariantList l;
    for (const RoiResult& r : resGui_) l << QVariantMap{{"id", r.id}, {"f", r.tone.f}, {"snr", r.tone.snr}, {"ok", r.ok}};
    return l;
}

void Player::fillPlot(const QString& kind, hz::PlotData& pd) {
    MeasureSettings ms;
    double win;
    {
        std::lock_guard<std::mutex> lk(mx_);
        ms = ms_;
        win = plotWin_;
    }
    const int sel = roiCtl_ ? roiCtl_->roiSet().selected : -1;
    if (kind == "play:spectrum") {
        pd.xlabel = "frequency (Hz)";
        pd.ylabel = "amplitude (rel.)";
        pd.xunit = "Hz";
        pd.empty = !clipGui_ ? "Open a recording" : resGui_.empty() ? "Draw or detect ROIs: their spectrum follows the playhead" : "–";
        pd.yFromZero = true;
        pd.fixX = true;
        pd.x0 = 0;
        pd.x1 = ms.fmax;
        for (const RoiResult& r : resGui_) {
            if (r.f.empty()) continue;
            hz::PlotSeries s{hz::PlotSeries::Line, r.name, r.color};
            s.width = r.id == sel ? 2.0f : 1.2f;
            s.x = r.f;
            s.y = r.amp;
            pd.series.push_back(std::move(s));
            if (r.ok) pd.texts.push_back({r.tone.f, r.tone.amp, QString("%1 %2 Hz").arg(r.name).arg(r.tone.f, 0, 'f', 1), r.color});
        }
        pd.legend = pd.series.size() <= 6;
        for (double f : {ms.toneLo, ms.toneHi}) {
            hz::PlotMark m;
            m.v = f;
            m.color = QColor(76, 155, 255, 150);
            m.dashed = true;
            pd.marks.push_back(m);
        }
    } else if (kind == "play:signal") {
        pd.xlabel = "time before the playhead (ms)";
        pd.ylabel = ms.signal == "displacement" ? "integrated ON−OFF (rel.)" : "ON−OFF per bin (rel.)";
        pd.xunit = "ms";
        pd.empty = resGui_.empty() ? "Draw or detect ROIs: their signal follows the playhead" : "–";
        pd.fixX = true;
        pd.x0 = -win * 1000;
        pd.x1 = 0;
        for (size_t i = 0; i < sigGui_.size() && i < resGui_.size(); ++i) {
            const auto& v = sigGui_[i];
            if (v.empty()) continue;
            hz::PlotSeries s{hz::PlotSeries::Line, resGui_[i].name, resGui_[i].color};
            s.width = resGui_[i].id == sel ? 1.6f : 1.0f;
            const int n = (int)v.size();
            for (int k = 0; k < n; ++k) { s.x.push_back((k + 1 - n) / ms.fs * 1000); s.y.push_back(v[k]); }
            pd.series.push_back(std::move(s));
        }
        pd.legend = pd.series.size() <= 6;
    }
}

// ---- export --------------------------------------------------------------------------------------------------------------------
static void roiPath(QPainterPath& path, const Roi& r) {
    if (r.type == RoiType::Line && r.pts.size() >= 2) {
        const QPointF p0 = r.pts[0], p1 = r.pts[1];
        const double L = std::hypot(p1.x() - p0.x(), p1.y() - p0.y());
        if (L < 0.5) return;
        const QPointF nrm(-(p1.y() - p0.y()) / L * r.half, (p1.x() - p0.x()) / L * r.half);
        path.addPolygon(QPolygonF({p0 + nrm, p1 + nrm, p1 - nrm, p0 - nrm, p0 + nrm}));
    } else if (r.type == RoiType::Rect && r.pts.size() >= 2) {
        path.addRect(QRectF(r.pts[0], r.pts[1]).normalized());
    } else if (r.type == RoiType::Polygon && r.pts.size() >= 3) {
        QPolygonF poly(QList<QPointF>(r.pts.begin(), r.pts.end()));
        poly << r.pts[0];
        path.addPolygon(poly);
    }
}

void Player::drawOverlay(QImage& img, const std::vector<Roi>& rois, const PlayParams& p, double t, double a, double b, const QString& clipName) {
    QPainter g(&img);
    g.setRenderHint(QPainter::Antialiasing);
    g.setRenderHint(QPainter::TextAntialiasing);
    for (const Roi& r : rois) {
        QPainterPath path;
        roiPath(path, r);
        if (path.isEmpty()) continue;
        g.setPen(QPen(r.color, 1.6));
        g.setBrush(Qt::NoBrush);
        g.drawPath(path);
        QFont lf = g.font();
        lf.setPixelSize(std::max(12, img.height() / 50));
        lf.setBold(true);
        g.setFont(lf);
        g.setPen(r.color);
        g.drawText(path.boundingRect().topLeft() + QPointF(0, -5), r.name);
    }
    const int H = img.height(), W = img.width();
    QFont f = g.font();
    f.setBold(false);
    f.setPixelSize(std::max(13, H / 40));
    g.setFont(f);
    const QFontMetrics fm(f);
    QString l1 = p.strobe ? QString("STROBE  %1 Hz  ·  drift %2 % per frame").arg(p.strobeHz, 0, 'f', 2).arg(p.strobeDrift, 0, 'f', 1)
                          : (p.speed < 1 ? QString("SLOW MOTION  ×%1").arg(1.0 / p.speed, 0, 'f', 0) : QString("SPEED  ×%1").arg(p.speed, 0, 'g', 3));
    const QString l2 = p.mode == "trail" ? QString("trail %1 ms (colour = time since the last event)").arg(p.trailMs, 0, 'g', 3)
                                         : QString("exposure %1 µs  =  %2 fps camera, no motion blur").arg(p.exposureUs, 0, 'f', 0).arg(1e6 / p.exposureUs, 0, 'f', 0);
    const QString l3 = QString("t = %1 ms   ·   %2").arg((t - a) * 1000, 0, 'f', 3).arg(clipName);
    const int lh = fm.height() + 4, bw = std::max({fm.horizontalAdvance(l1), fm.horizontalAdvance(l2), fm.horizontalAdvance(l3)}) + 28;
    const QRect box(16, H - 3 * lh - 34, bw, 3 * lh + 14);
    g.setPen(Qt::NoPen);
    g.setBrush(QColor(10, 11, 14, 200));
    g.drawRoundedRect(box, 8, 8);
    g.setPen(QColor(255, 79, 176));
    QFont fb = f;
    fb.setBold(true);
    g.setFont(fb);
    g.drawText(box.left() + 14, box.top() + 7 + fm.ascent(), l1);
    g.setFont(f);
    g.setPen(QColor(230, 232, 235));
    g.drawText(box.left() + 14, box.top() + 7 + lh + fm.ascent(), l2);
    g.setPen(QColor(155, 161, 171));
    g.drawText(box.left() + 14, box.top() + 7 + 2 * lh + fm.ascent(), l3);
    // progress through the exported range
    const double fr = b > a ? std::clamp((t - a) / (b - a), 0.0, 1.0) : 0;
    g.fillRect(QRectF(0, H - 6, W, 6), QColor(255, 255, 255, 30));
    g.fillRect(QRectF(0, H - 6, W * fr, 6), QColor(255, 79, 176));
}

void Player::exportVideo(double fps) {
    if (!clipGui_ || exBusy_) return;
    fps = std::clamp(fps, 5.0, 120.0);
    const PlayParams p = params();
    double a, b;
    {
        std::lock_guard<std::mutex> lk(mx_);
        range(a, b);
    }
    const double step = p.strobe ? (1.0 / p.strobeHz) * (1 + p.strobeDrift / 100) : p.speed / fps;
    const double frames = (b - a) / step;
    if (frames > 36000) {
        emit toast(QString("That would be %1 frames (%2 min of video): shorten the window, or play faster").arg(frames, 0, 'f', 0).arg(frames / fps / 60, 0, 'f', 1));
        return;
    }
    if (frames < 2) { emit toast("The range is shorter than two frames"); return; }
    const QFileInfo fi(clipGui_->path);
    const QString dir = QDir(fi.absolutePath()).filePath(fi.completeBaseName() + ".video");
    QDir().mkpath(dir);
    const QString tag = p.strobe ? QString("strobe%1Hz").arg(p.strobeHz, 0, 'f', 1) : QString("x%1").arg(1.0 / p.speed, 0, 'f', 0);
    const QString out = QDir(dir).filePath(QString("%1_%2_%3_%4").arg(fi.completeBaseName(), tag, p.mode, QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")));
    // a portable package ships ffmpeg next to hertzyevs.exe; otherwise the one on the PATH
    QString ffmpeg = QStandardPaths::findExecutable("ffmpeg", {QCoreApplication::applicationDirPath()});
    if (ffmpeg.isEmpty()) ffmpeg = QStandardPaths::findExecutable("ffmpeg");
    exCancel_ = false;
    exProgress_ = 0;
    exBusy_ = true;
    exp_["text"] = QString("Rendering %1 frames…").arg(frames, 0, 'f', 0);
    emit exportChanged();
    if (exTh_.joinable()) exTh_.join();
    exTh_ = std::thread([this, c = clipGui_, p, a, b, fps, out, ffmpeg] { exportRun(c, p, a, b, fps, out, ffmpeg); });
}

void Player::cancelExport() { exCancel_ = true; }

void Player::exportRun(std::shared_ptr<const Clip> c, PlayParams p, double a, double b, double fps, QString out, QString ffmpeg) {
    std::vector<Roi> rois;
    {
        std::lock_guard<std::mutex> lk(mx_);
        rois = rois_;
    }
    FrameRenderer r;
    const double step = p.strobe ? (1.0 / p.strobeHz) * (1 + p.strobeDrift / 100) : p.speed / fps;
    const double t0 = a + (p.strobe ? 0.0 : std::min(p.exposureUs * 1e-6, (b - a) / 4));
    const int n = std::max(2, (int)std::floor((b - t0) / step) + 1);
    const int W = c->W & ~1, H = c->H & ~1;
    QString msg;
    QProcess proc;
    const bool mp4 = !ffmpeg.isEmpty();
    QString target = mp4 ? out + ".mp4" : out;
    if (mp4) {
        proc.start(ffmpeg, {"-y", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "bgra", "-s", QString("%1x%2").arg(W).arg(H), "-r", QString::number(fps),
                            "-i", "-", "-c:v", "libx264", "-preset", "medium", "-crf", "18", "-pix_fmt", "yuv420p", "-movflags", "+faststart", target});
        if (!proc.waitForStarted(5000)) msg = "Could not start ffmpeg: " + proc.errorString();
    } else {
        QDir().mkpath(target);
    }
    int k = 0;
    for (; msg.isEmpty() && k < n && !exCancel_; ++k) {
        const double t = std::min(b, t0 + k * step);
        QImage img = r.render(*c, p, t);
        drawOverlay(img, rois, p, t, a, b, c->name);
        if (img.width() != W || img.height() != H) img = img.copy(0, 0, W, H);
        if (mp4) {
            proc.write(reinterpret_cast<const char*>(img.constBits()), img.sizeInBytes());
            while (proc.bytesToWrite() > (16 << 20) && proc.state() == QProcess::Running) proc.waitForBytesWritten(1000);
            if (proc.state() != QProcess::Running) msg = "ffmpeg stopped: " + QString::fromUtf8(proc.readAllStandardError()).trimmed();
        } else if (!img.save(QDir(target).filePath(QString("frame_%1.png").arg(k, 6, 10, QChar('0'))))) {
            msg = "Could not write " + target;
        }
        exProgress_ = double(k + 1) / n;
    }
    if (mp4) {
        proc.closeWriteChannel();
        if (exCancel_) proc.kill();
        proc.waitForFinished(-1);
        if (msg.isEmpty() && !exCancel_ && (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0))
            msg = "ffmpeg failed: " + QString::fromUtf8(proc.readAllStandardError()).trimmed();
    }
    if (msg.isEmpty())
        msg = exCancel_ ? QString("Export cancelled")
                        : mp4 ? QString("Video saved: %1 (%2 frames, %3 s)").arg(target).arg(k).arg(k / fps, 0, 'f', 1)
                              : QString("ffmpeg not found: %1 PNG frames saved in %2 (install ffmpeg for MP4)").arg(k).arg(target);
    exBusy_ = false;
    std::lock_guard<std::mutex> lk(exmx_);
    exMsg_ = msg;
    exDone_ = true;
}

}  // namespace evs

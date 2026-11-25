// Simulated Triton2 EVS looking at a lab bench, so the app can be used and tested without the camera:
//  - left: dark round bars (2 mm) on a lighter background that ring laterally at known tones (kSimBarHz) after every
//    tap, plus a slow rocking of the whole stand. Each edge pixel runs the approximate IMX636 pixel model of
//    pixelmodel.hpp (log intensity, front-end low-pass (fo), high-pass (hpf), ON/OFF thresholds (diff_on/off),
//    refractory period (refr), threshold mismatch and noise).
//  - right: a fan with dark blades turning at a set speed (with a slow +-0.5 % wander) and a bright tape mark on the tip
//    of blade 1. Its edges are generated analytically: every blade edge that crosses a pixel gives the events of that
//    contrast step, spread over the time the blurred edge needs to pass.
// Plus random background events. Output: EVT3 chunks, like the camera's encoded stream.
#include <QMutex>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <random>
#include <thread>
#include "pixelmodel.hpp"
#include "source.hpp"

namespace evs {
namespace {

struct Rng {   // xorshift64*
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 1) {}
    uint64_t next() { s ^= s >> 12; s ^= s << 25; s ^= s >> 27; return s * 0x2545F4914F6CDD1Dull; }
    double uni() { return (next() >> 11) * (1.0 / 9007199254740992.0); }
    double gauss() { return (uni() + uni() + uni() + uni() - 2.0) * 1.7320508; }   // ~N(0,1)
};

struct Params {
    double scale = 0.10;        // mm per pixel on the bars
    double contrast = 2.0;      // background / bar (and blade) brightness
    double blur = 0.6;          // optical blur sigma (px)
    double noiseHz = 0.05;      // background events per pixel per second
    double lightNoise = 0.012;  // log-intensity noise per pixel per step (shot noise)
    double tapEvery = 4.0;      // s between automatic taps (0 = only on request)
    double rock = 1.5;          // 26 Hz rocking amplitude relative to the bar tone
    bool fan = true;            // the fan on the right
    double fanRpm = 600;        // set speed
    int blades = 3;
    bool tape = true;           // bright tape on the tip of blade 1 (one mark per turn)
    std::map<QString, QVariant> feat;
    double f(const char* k, double def = 0) const { auto it = feat.find(k); return it == feat.end() ? def : it->second.toDouble(); }
    bool b(const char* k) const { auto it = feat.find(k); return it != feat.end() && it->second.toBool(); }
};

struct Pixel {
    uint16_t x, y;
    float d;                // signed distance of the pixel centre from the bar axis at rest (px)
    float lp, slow, ref, vprev;
    float thOn, thOff;      // per-pixel thresholds (mismatch)
    double tlast;
};

struct Bar {
    double x0, tilt, f, amp_px, phase, r_px;
    std::vector<Pixel> px;
    std::vector<float> lut;     // log intensity vs (d - u)
    double lut_d0 = 0, lut_inv = 32;
};

struct FanPixel {
    uint16_t x, y;
    float ang, r;           // angle (rad) and radius (px) about the hub
    float thOn, thOff;
    bool tape;              // inside the tape zone of blade 1
};

class SimSource : public EventSource {
public:
    SimSource() {
        p_.feat = {{"bias_diff_on", 0}, {"bias_diff_off", 0}, {"bias_fo", 0}, {"bias_hpf", 0}, {"bias_refr", 0},
                   {"erc_enable", false}, {"erc_rate", 40.0}, {"afk_enable", false}, {"afk_mode", "BandStop"}, {"afk_low", 50},
                   {"afk_high", 99}, {"trail_enable", false}, {"roi_enable", false}, {"roi_x", 0}, {"roi_y", 0},
                   {"roi_w", 1280}, {"roi_h", 720}};
    }
    ~SimSource() override { stop(); }
    QString kind() const override { return "sim"; }
    QString description() const override { return "Simulated Triton2 EVS (lab bench: ringing bars + fan)"; }
    bool encoded() const override { return true; }
    bool running() const override { return running_; }
    bool open(QString*) override { return true; }
    bool start(QString*) override {
        if (running_) return true;
        running_ = true;
        th_ = std::thread([this] { loop(); });
        return true;
    }
    void stop() override {
        running_ = false;
        if (th_.joinable()) th_.join();
    }
    bool setFeature(const QString& key, const QVariant& v, QString*) override {
        std::lock_guard<std::mutex> lk(mx_);
        p_.feat[key] = v;
        dirty_ = true;
        return true;
    }
    bool getFeature(const QString& key, QVariant& v) override {
        std::lock_guard<std::mutex> lk(mx_);
        auto it = p_.feat.find(key);
        if (it == p_.feat.end()) return false;
        v = it->second;
        return true;
    }
    QString nodeFor(const QString& key) const override { return "sim:" + key; }
    std::vector<NodeInfo> nodes(const QString& filter) override {
        std::vector<NodeInfo> out;
        std::lock_guard<std::mutex> lk(mx_);
        for (const auto& kv : p_.feat) {
            if (!filter.isEmpty() && !kv.first.contains(filter, Qt::CaseInsensitive)) continue;
            NodeInfo n;
            n.name = "sim:" + kv.first;
            n.display = kv.first;
            n.type = kv.second.typeId() == QMetaType::Bool ? "Boolean" : kv.second.typeId() == QMetaType::QString ? "Enum" : "Float";
            n.value = kv.second.toString();
            n.readable = n.writable = true;
            out.push_back(n);
        }
        return out;
    }
    bool setNode(const QString& name, const QString& value, QString* err) override {
        QString k = name;
        k.remove("sim:");
        return setFeature(k, value, err);
    }
    QVariantMap stats() override {
        std::lock_guard<std::mutex> lk(mx_);
        return stats_;
    }
    void tap() override { tap_req_ = true; }
    void setSimParam(const QString& key, const QVariant& v) override {
        std::lock_guard<std::mutex> lk(mx_);
        if (key == "scale") p_.scale = std::clamp(v.toDouble(), 0.01, 2.0);
        else if (key == "contrast") p_.contrast = std::clamp(v.toDouble(), 1.05, 50.0);
        else if (key == "blur") p_.blur = std::clamp(v.toDouble(), 0.3, 5.0);
        else if (key == "noiseHz") p_.noiseHz = std::clamp(v.toDouble(), 0.0, 50.0);
        else if (key == "tapEvery") p_.tapEvery = std::max(0.0, v.toDouble());
        else if (key == "rock") p_.rock = std::max(0.0, v.toDouble());
        else if (key == "fan") p_.fan = v.toBool();
        else if (key == "fanRpm") p_.fanRpm = std::clamp(v.toDouble(), 0.0, 30000.0);
        else if (key == "blades") p_.blades = std::clamp(v.toInt(), 1, 12);
        else if (key == "tape") p_.tape = v.toBool();
        dirty_ = true;
    }

private:
    void build(const Params& p) {
        bars_.clear();
        // bars in the left half, ~16 mm apart at 0.1 mm/px; the fan sits in the right half
        const double area = kSimBarArea * width, width_mm = area * p.scale;
        const int ns = std::clamp((int)std::lround(width_mm / 16.0), 1, kSimBars);
        const double sigma = std::sqrt(p.blur * p.blur + 1.0 / 12.0);
        Rng rng(7);
        for (int i = 0; i < ns; ++i) {
            Bar s;
            s.x0 = (i + 0.5) / ns * area;
            s.tilt = (i - (ns - 1) / 2.0) * 4.5 * M_PI / 180;
            s.f = kSimBarHz[i];
            s.amp_px = kSimBarAmpUm[i] / (p.scale * 1000.0);
            s.phase = rng.uni() * 2 * M_PI;
            s.r_px = 1.0 / p.scale;   // 2 mm bar
            const double move = s.amp_px * (1 + p.rock) + 0.5;
            const double m = std::min(3.0 * sigma + move + 1.0, 40.0);
            // log-intensity profile across the bar (blurred), sampled every 1/32 px
            const double span = s.r_px + m + move + 2;
            s.lut_d0 = -span;
            s.lut_inv = 32;
            const int nl = (int)(2 * span * 32) + 2;
            s.lut.resize(nl);
            const double dark = 1.0 / p.contrast;
            for (int k = 0; k < nl; ++k) {
                const double d = -span + k / 32.0;
                const double cov = 0.5 * (std::erf((d + s.r_px) / (M_SQRT2 * sigma)) - std::erf((d - s.r_px) / (M_SQRT2 * sigma)));
                s.lut[k] = (float)std::log(1.0 - (1.0 - dark) * cov);
            }
            const double c = std::cos(s.tilt), tn = std::tan(s.tilt);
            std::vector<int> xs;
            for (int y = 0; y < height; ++y) {
                const double xc = s.x0 + (y - height / 2.0) * tn;
                xs.clear();
                for (int side : {-1, 1}) {   // pixels around both edges (once each when the bar is thin)
                    const double e = xc + side * s.r_px / c;
                    for (int x = (int)std::floor(e - m / c); x <= (int)std::ceil(e + m / c); ++x)
                        if (x >= 0 && x < width) xs.push_back(x);
                }
                std::sort(xs.begin(), xs.end());
                xs.erase(std::unique(xs.begin(), xs.end()), xs.end());
                {
                    for (int x : xs) {
                        Pixel px{};
                        px.x = (uint16_t)x;
                        px.y = (uint16_t)y;
                        px.d = (float)((x + 0.5 - xc) * c);
                        const double l = look(s, px.d);
                        px.lp = px.slow = (float)l;
                        px.ref = 0;
                        px.vprev = 0;
                        px.thOn = (float)(1 + 0.04 * rng.gauss());
                        px.thOff = (float)(1 + 0.04 * rng.gauss());
                        px.tlast = -1;
                        s.px.push_back(px);
                    }
                }
            }
            bars_.push_back(std::move(s));
        }
        // fan pixels: the blade annulus around the hub
        fanpx_.clear();
        if (p.fan) {
            const double cx = kSimFanX * width, cy = height / 2.0;
            for (int y = (int)(cy - kSimFanR) - 1; y <= (int)(cy + kSimFanR) + 1; ++y)
                for (int x = (int)(cx - kSimFanR) - 1; x <= (int)(cx + kSimFanR) + 1; ++x) {
                    if (x < 0 || y < 0 || x >= width || y >= height) continue;
                    const double dx = x + 0.5 - cx, dy = y + 0.5 - cy, r = std::hypot(dx, dy);
                    if (r < kSimFanHub || r > kSimFanR) continue;
                    FanPixel q{(uint16_t)x, (uint16_t)y, (float)std::atan2(dy, dx), (float)r,
                               (float)(1 + 0.04 * rng.gauss()), (float)(1 + 0.04 * rng.gauss()), p.tape && r > kSimTapeFrom * kSimFanR};
                    fanpx_.push_back(q);
                }
        }
    }
    // events of the fan over [t, t + dur): blade edges crossing each pixel, phase phi0 at t, angular speed w (rad/s)
    void fanEvents(const Params& p, double t, double dur, double phi0, double w, double thOn, double thOff, Rng& r,
                   const std::function<bool(int, int)>& inRoi, std::vector<Ev>& out) const {
        if (fanpx_.empty() || w <= 0) return;
        const int nb = std::max(1, p.blades);
        const double pitch = 2 * M_PI / nb, hw = 0.5 * kSimBladeDuty * pitch;
        const double lnDark = std::log(kSimBladeContrast), lnTape = std::log(kSimTapeGain);
        const double dphi = w * dur;
        const double sigma = std::sqrt(p.blur * p.blur + 1.0 / 12.0);
        for (const FanPixel& q : fanpx_) {
            if (!inRoi(q.x, q.y)) continue;
            const double spread = 2.5 * sigma / q.r / w;   // time the blurred edge needs to pass (s)
            for (int k = 0; k < nb; ++k) {
                const bool tape = q.tape && k == 0;
                const double L = tape ? lnTape : lnDark;
                for (int e = 0; e < 2; ++e) {
                    // e = 0: leading edge (the pixel enters the blade), 1: trailing edge (it leaves the blade)
                    const double edgeOff = k * pitch + (e == 0 ? hw : -hw);
                    double a = std::fmod(q.ang - edgeOff - phi0, 2 * M_PI);
                    if (a < 0) a += 2 * M_PI;
                    for (; a < dphi; a += 2 * M_PI) {
                        const double te = t + a / w;
                        // entering a dark blade darkens (OFF), entering the tape brightens (ON); leaving is the reverse
                        const bool on = tape ? e == 0 : e == 1;
                        const double th = on ? thOn * q.thOn : thOff * q.thOff;
                        const int n = (int)std::floor(L / th + r.uni());
                        for (int j = 0; j < n; ++j) {
                            const double tj = std::clamp(te + ((j + 0.5) / n - 0.5) * spread + 0.15 * spread * r.gauss() / n, t, t + dur - 1e-6);
                            out.push_back(Ev{q.x, q.y, (uint8_t)(on ? 1 : 0), (int64_t)std::llround(tj * 1e6)});
                        }
                    }
                }
            }
        }
    }
    static double look(const Bar& s, double d) {
        double k = (d - s.lut_d0) * s.lut_inv;
        if (k <= 0) return s.lut.front();
        const int i = (int)k;
        if (i >= (int)s.lut.size() - 1) return s.lut.back();
        const double fr = k - i;
        return s.lut[i] * (1 - fr) + s.lut[i + 1] * fr;
    }

    void loop() {
        const double dt = 100e-6;            // pixel model step
        const int steps = 100;               // 10 ms chunks
        double t = 0, t_tap = -1e9, next_tap = 1.0, phi = 0;
        Params p;
        Rng noise(99);
        Evt3Encoder enc;
        const auto wall0 = std::chrono::steady_clock::now();
        double slowest = 1.0;
        std::vector<std::vector<Ev>> per(8);
        while (running_) {
            bool rebuild = false;
            {
                std::lock_guard<std::mutex> lk(mx_);
                if (dirty_) {
                    const bool geo = p.scale != p_.scale || p.contrast != p_.contrast || p.blur != p_.blur || p.rock != p_.rock ||
                                     p.fan != p_.fan || p.tape != p_.tape || bars_.empty();
                    p = p_; rebuild = geo; dirty_ = false;
                }
            }
            if (rebuild || bars_.empty()) build(p);
            if (tap_req_.exchange(false) || (p.tapEvery > 0 && t >= next_tap)) {
                t_tap = t;
                next_tap = t + (p.tapEvery > 0 ? p.tapEvery : 1e9);
                for (auto& s : bars_) s.phase = noise.uni() * 2 * M_PI;
            }
            const double thOn = theta_on(p.f("bias_diff_on")), thOff = theta_off(p.f("bias_diff_off"));
            const double aLp = 1 - std::exp(-2 * M_PI * fo_corner_hz(p.f("bias_fo")) * dt);
            const double aHp = 1 - std::exp(-2 * M_PI * hpf_corner_hz(p.f("bias_hpf")) * dt);
            const double refr = refractory_us(p.f("bias_refr")) * 1e-6;
            const bool roi = p.b("roi_enable");
            const int rx = (int)p.f("roi_x"), ry = (int)p.f("roi_y"), rw = (int)p.f("roi_w", width), rh = (int)p.f("roi_h", height);
            auto inRoi = [&](int x, int y) { return !roi || (x >= rx && x < rx + rw && y >= ry && y < ry + rh); };
            const double tChunk = t;
            const auto c0 = std::chrono::steady_clock::now();

            // bars in parallel, the fan on one more thread
            const int ns = (int)bars_.size();
            if ((int)per.size() < ns + 1) per.resize(ns + 1);
            const double rps = p.fanRpm / 60.0 * (1 + kSimFanWander * std::sin(2 * M_PI * 0.2 * (tChunk + steps * dt / 2)));
            const double wf = 2 * M_PI * rps, phi0 = phi;
            phi = std::fmod(phi + wf * steps * dt, 2 * M_PI);
            fan_rps_ = rps;
            std::vector<std::thread> th;
            for (int si = 0; si < ns; ++si) {
                th.emplace_back([&, si] {
                    Bar& s = bars_[si];
                    std::vector<Ev>& out = per[si];
                    out.clear();
                    Rng r(uint64_t(tChunk * 1e6) * 31 + si);
                    const double w = 2 * M_PI * s.f, decay = kSimBarZeta * w;
                    for (int k = 0; k < steps; ++k) {
                        const double tk = tChunk + (k + 1) * dt, ta = tk - t_tap;
                        double u = 0;
                        if (ta >= 0) {
                            u = s.amp_px * std::exp(-decay * ta) * std::sin(w * ta + s.phase)
                              + s.amp_px * p.rock * std::exp(-ta / 0.4) * std::sin(2 * M_PI * 26.0 * ta);
                        }
                        for (Pixel& q : s.px) {
                            const double l = look(s, q.d - u) + p.lightNoise * r.gauss();
                            q.lp += (float)(aLp * (l - q.lp));
                            q.slow += (float)(aHp * (q.lp - q.slow));
                            const float v = q.lp - q.slow;
                            for (int guard = 0; guard < 6; ++guard) {
                                const float on = (float)(thOn * q.thOn), off = (float)(thOff * q.thOff);
                                int pol = -1;
                                float level = 0;
                                if (v - q.ref >= on) { pol = 1; level = q.ref + on; }
                                else if (q.ref - v >= off) { pol = 0; level = q.ref - off; }
                                if (pol < 0) break;
                                // crossing time inside the step
                                const float dv = v - q.vprev;
                                const double fr = std::fabs(dv) > 1e-9f ? std::clamp((double)(level - q.vprev) / dv, 0.0, 1.0) : 1.0;
                                const double te = tk - dt + fr * dt;
                                if (te - q.tlast < refr) { q.ref = v; break; }   // still refractory: the pixel resets
                                q.ref = level;
                                q.tlast = te;
                                if (inRoi(q.x, q.y)) out.push_back(Ev{q.x, q.y, (uint8_t)pol, (int64_t)std::llround(te * 1e6)});
                            }
                            q.vprev = v;
                        }
                    }
                });
            }
            th.emplace_back([&] {
                std::vector<Ev>& out = per[ns];
                out.clear();
                Rng r(uint64_t(tChunk * 1e6) * 17 + 977);
                fanEvents(p, tChunk, steps * dt, phi0, wf, thOn, thOff, r, inRoi, out);
            });
            for (auto& x : th) x.join();

            std::vector<Ev> ev;
            size_t total = 0;
            for (int si = 0; si <= ns; ++si) total += per[si].size();
            ev.reserve(total + 4096);
            for (int si = 0; si <= ns; ++si) ev.insert(ev.end(), per[si].begin(), per[si].end());
            // background activity
            const double lam = p.noiseHz * width * height * steps * dt;
            std::poisson_distribution<int> pd(std::max(lam, 0.0));
            std::mt19937 g((unsigned)(tChunk * 1e3) + 5);
            const int nn = lam > 0 ? pd(g) : 0;
            for (int i = 0; i < nn; ++i) {
                const int x = std::min(width - 1, (int)(noise.uni() * width)), y = std::min(height - 1, (int)(noise.uni() * height));
                if (inRoi(x, y)) ev.push_back(Ev{(uint16_t)x, (uint16_t)y, (uint8_t)(noise.next() & 1), (int64_t)std::llround((tChunk + noise.uni() * steps * dt) * 1e6)});
            }
            std::sort(ev.begin(), ev.end(), [](const Ev& a, const Ev& b) { return a.t < b.t; });
            // event rate controller: thin out uniformly above the limit
            if (p.b("erc_enable")) {
                const double cap = p.f("erc_rate", 40) * 1e6 * steps * dt;
                if (ev.size() > cap && cap > 0) {
                    const double keep = cap / ev.size();
                    std::vector<Ev> kept;
                    kept.reserve((size_t)cap + 16);
                    for (const Ev& e : ev) if (noise.uni() < keep) kept.push_back(e);
                    ev.swap(kept);
                }
            }
            t += steps * dt;
            Chunk c;
            if (!ev.empty()) {
                std::vector<uint16_t> words;
                enc.encode(ev.data(), ev.size(), words);
                c.raw.resize(words.size() * 2);
                std::memcpy(c.raw.data(), words.data(), c.raw.size());
            } else {
                // keep the time base moving: a lone TIME_HIGH
                const uint16_t w = uint16_t(0x8000 | ((int64_t(t * 1e6) >> 12) & 0xFFF));
                c.raw.resize(2);
                std::memcpy(c.raw.data(), &w, 2);
            }
            const double cost = std::chrono::duration<double>(std::chrono::steady_clock::now() - c0).count();
            slowest = 0.9 * slowest + 0.1 * std::min(1.0, (steps * dt) / std::max(cost, 1e-6));
            {
                std::lock_guard<std::mutex> lk(mx_);
                stats_["cameraRate"] = ev.size() / (steps * dt);
                stats_["linkBps"] = c.raw.size() / (steps * dt);
                stats_["realtime"] = slowest;
                stats_["simTime"] = t;
                stats_["bars"] = (int)bars_.size();
                stats_["fanRpm"] = p.fan ? fan_rps_ * 60 : 0.0;
                stats_["lastTap"] = t_tap;
            }
            if (sink) sink(std::move(c));
            // pace to the wall clock
            const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - wall0).count();
            if (t > wall + 0.002) std::this_thread::sleep_for(std::chrono::duration<double>(t - wall));
        }
    }

    std::thread th_;
    std::atomic<bool> running_{false}, tap_req_{false};
    std::mutex mx_;
    Params p_;
    bool dirty_ = true;
    std::vector<Bar> bars_;
    std::vector<FanPixel> fanpx_;
    double fan_rps_ = 0;
    QVariantMap stats_;
};

}  // namespace

std::unique_ptr<EventSource> make_sim_source() { return std::make_unique<SimSource>(); }

}  // namespace evs

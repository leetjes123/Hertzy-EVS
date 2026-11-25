#include "engine.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>
#include "prof.hpp"

namespace evs {

std::vector<uint16_t> build_probe_lut(int w, int h, const std::vector<Probe>& probes) {
    std::vector<uint16_t> lut((size_t)w * h, 0);
    for (size_t k = 0; k < probes.size() && k < 30000; ++k) {
        const Probe& p = probes[k];
        const double L = p.length();
        if (L < 2) continue;
        const double ux = (p.x1 - p.x0) / L, uy = (p.y1 - p.y0) / L, nx = -uy, ny = ux;
        const double pad = p.half + 1;
        const int xa = std::max(0, (int)std::floor(std::min(p.x0, p.x1) - pad)), xb = std::min(w - 1, (int)std::ceil(std::max(p.x0, p.x1) + pad));
        const int ya = std::max(0, (int)std::floor(std::min(p.y0, p.y1) - pad)), yb = std::min(h - 1, (int)std::ceil(std::max(p.y0, p.y1) + pad));
        for (int y = ya; y <= yb; ++y)
            for (int x = xa; x <= xb; ++x) {
                const double dx = x + 0.5 - p.x0, dy = y + 0.5 - p.y0;
                const double s = dx * ux + dy * uy, d = dx * nx + dy * ny;
                if (s < 0 || s > L || std::fabs(d) > p.half) continue;
                uint16_t& v = lut[(size_t)y * w + x];
                if (v == 0) v = uint16_t(2 * k + (d >= 0 ? 0 : 1) + 1);
            }
    }
    return lut;
}

Engine::Engine(int w, int h) : w_(w), h_(h) {
    last_t_.assign((size_t)w * h, INT64_MIN / 4);
    last_p_.assign((size_t)w * h, 0);
    act_.assign((size_t)w * h, 0.f);
    rate_all_.assign(nrate_, 0.f);
    rate_roi_.assign(nrate_, 0.f);
    lut_.assign((size_t)w * h, 0);
    sign_.assign((size_t)w * h, 0);
    roi_mask_.assign((size_t)w * h, 1);
    rate_clock_ = std::chrono::steady_clock::now();
    th_ = std::thread([this] { loop(); });
    wth_ = std::thread([this] { writerLoop(); });
}

Engine::~Engine() {
    quit_ = true;
    qcv_.notify_all();
    wcv_.notify_all();
    if (th_.joinable()) th_.join();
    if (wth_.joinable()) wth_.join();
    if (wf_) fclose(wf_);
}

void Engine::push(Chunk&& c) {
    const size_t sz = c.raw.size() + c.ev.size() * sizeof(Ev);
    {
        std::lock_guard<std::mutex> lk(qmx_);
        // a backlog of > 3 GB means processing can't keep up at all: drop instead of exhausting the memory
        if (q_bytes_ + sz > (size_t(3) << 30)) { ++dropped_chunks_; return; }
        q_bytes_ += sz;
        q_.push_back(std::move(c));
    }
    qcv_.notify_one();
}

void Engine::reset() {
    std::lock_guard<std::mutex> lk(qmx_);
    q_.clear();
    q_bytes_ = 0;
    reset_req_ = true;
    qcv_.notify_one();
}

void Engine::setProbes(const std::vector<Probe>& p) {
    const auto pl = build_probe_lut(w_, h_, p);
    std::vector<uint16_t> lut(pl.size(), 0);
    std::vector<int8_t> sign(pl.size(), 0);
    for (size_t i = 0; i < pl.size(); ++i) {
        if (!pl[i]) continue;
        lut[i] = uint16_t(((pl[i] - 1) >> 1) + 1);
        sign[i] = ((pl[i] - 1) & 1) ? 1 : -1;   // -normal side counts +, +normal side counts -
    }
    setRois(std::move(lut), std::move(sign), (int)p.size());
}

void Engine::setRois(std::vector<uint16_t> lut, std::vector<int8_t> sign, int n) {
    if (lut.size() != (size_t)w_ * h_ || sign.size() != lut.size()) { lut.assign((size_t)w_ * h_, 0); sign.assign(lut.size(), 0); n = 0; }
    std::lock_guard<std::mutex> lk(dmx_);
    nroi_ = n;
    lut_.swap(lut);
    sign_.swap(sign);
    r_on_.assign(n, std::vector<float>(nbins_, 0.f));
    r_off_.assign(n, std::vector<float>(nbins_, 0.f));
    r_n_.assign(n, std::vector<float>(nbins_, 0.f));
}

void Engine::setMeasureTrigger(bool on, int source, double factor, double gapS, bool regionOnly) {
    mt_region_ = regionOnly;
    std::lock_guard<std::mutex> lk(mmx_);
    if (on && !mt_on_) mt_arm_ = -1;   // new baseline
    mt_on_ = on;
    mt_src_ = source;
    mt_factor_ = std::max(1.2, factor);
    mt_gap_ = std::max(0.01, gapS);
    if (!on) mt_q_.clear();
}

void Engine::takeMeasureTriggers(std::vector<int64_t>& out) {
    std::lock_guard<std::mutex> lk(mmx_);
    out.swap(mt_q_);
    mt_q_.clear();
}

// mmx_ held
void Engine::pushMeasureTrigger(int64_t t) {
    if (t - mt_last_ < (int64_t)(mt_gap_ * 1e6)) return;
    mt_last_ = t;
    mt_q_.push_back(t);
}

// same rule as the record trigger, but repeating: 3 ms rate in the trigger region > factor x the mean of the 0.5 s before
void Engine::checkMeasureTrigger(int64_t done) {
    std::lock_guard<std::mutex> lk(mmx_);
    if (!mt_on_ || mt_src_ != 0) return;
    if (mt_arm_ < 0) mt_arm_ = done;
    if (done - mt_arm_ < 600 || done < 600) return;
    const std::vector<float>& r = mt_region_ ? rate_roi_ : rate_all_;
    double cur = 0, base = 0;
    for (int64_t i = done - 2; i <= done; ++i) cur += r[i % nrate_];
    for (int64_t i = done - 510; i < done - 10; ++i) base += r[i % nrate_];
    cur *= 1000.0 / 3;
    base *= 1000.0 / 500;
    if (cur > mt_factor_ * std::max(base, 20000.0)) pushMeasureTrigger((done - 2) * 1000);
}

int64_t Engine::binTime() {
    std::lock_guard<std::mutex> lk(dmx_);
    return bin_hi_ < 0 ? -1 : bin_hi_ * kProbeBinUs;
}

void Engine::setSoftRoi(bool on, const QRectF& r) {
    std::vector<uint8_t> m((size_t)w_ * h_, on ? 0 : 1);
    if (on) {
        const int x0 = std::clamp((int)r.left(), 0, w_), x1 = std::clamp((int)std::ceil(r.right()), 0, w_);
        const int y0 = std::clamp((int)r.top(), 0, h_), y1 = std::clamp((int)std::ceil(r.bottom()), 0, h_);
        for (int y = y0; y < y1; ++y) std::memset(&m[(size_t)y * w_ + x0], 1, std::max(0, x1 - x0));
    }
    std::lock_guard<std::mutex> lk(dmx_);
    roi_on_ = on;
    roi_ = r;
    roi_mask_.swap(m);
}

bool Engine::frame(QImage& out, uint64_t& seq) {
    std::lock_guard<std::mutex> lk(fmx_);
    if (frame_seq_ == seq || frame_.isNull()) return false;
    out = frame_;
    seq = frame_seq_;
    return true;
}

// ---- processing thread ---------------------------------------------------------------------------------------------
void Engine::loop() {
    std::vector<Chunk> batch;
    while (!quit_) {
        {
            std::unique_lock<std::mutex> lk(qmx_);
            qcv_.wait_for(lk, std::chrono::milliseconds(5), [&] { return quit_ || !q_.empty() || reset_req_; });
            if (reset_req_) {
                reset_req_ = false;
                lk.unlock();
                dec_.reset();
                dec_.width = w_;
                dec_.height = h_;
                std::fill(last_t_.begin(), last_t_.end(), INT64_MIN / 4);
                std::fill(act_.begin(), act_.end(), 0.f);
                t_now_ = 0;
                pre_.clear();
                std::lock_guard<std::mutex> dl(dmx_);
                bin_hi_ = rbin_hi_ = -1;
                for (auto* r : {&r_on_, &r_off_, &r_n_}) for (auto& v : *r) std::fill(v.begin(), v.end(), 0.f);
                std::fill(rate_all_.begin(), rate_all_.end(), 0.f);
                std::fill(rate_roi_.begin(), rate_roi_.end(), 0.f);
                { std::lock_guard<std::mutex> ml(mmx_); mt_q_.clear(); mt_arm_ = -1; mt_last_ = INT64_MIN / 4; }
                continue;
            }
            while (!q_.empty() && batch.size() < 64) {
                q_bytes_ -= q_.front().raw.size() + q_.front().ev.size() * sizeof(Ev);
                batch.push_back(std::move(q_.front()));
                q_.pop_front();
            }
        }
        for (Chunk& c : batch) process(c);
        batch.clear();
        const auto now = std::chrono::steady_clock::now();
        const double sinceRender = std::chrono::duration<double>(now - last_render_).count();
        if (sinceRender >= 1.0 / std::max(5.0, fps_.load())) {
            // while the queue is backing up (high event rates) processing events matters more than drawing: skip the
            // picture, but still refresh twice a second so the view never freezes
            bool behind;
            { std::lock_guard<std::mutex> lk(qmx_); behind = q_bytes_ > (1 << 20); }
            if (!behind || sinceRender >= 0.5) {
                last_render_ = now;
                render();
            }
        }
        const double el = std::chrono::duration<double>(now - rate_clock_).count();
        if (el >= 0.25) {
            ev_rate_ = ev_since_ / el;
            ev_since_ = 0;
            rate_clock_ = now;
        }
    }
}

void Engine::process(Chunk& c) {
    hz::ProfScope prof_("engine.process");
    evbuf_.clear();
    std::vector<uint8_t> raw;
    if (!c.raw.empty()) {
        trig_buf_.clear();
        dec_.trigOut = &trig_buf_;
        dec_.decodeBytes(c.raw.data(), c.raw.size(), evbuf_);
        if (!trig_buf_.empty()) {
            std::lock_guard<std::mutex> ml(mmx_);
            if (mt_on_ && mt_src_ == 1)
                for (const TrigEv& t : trig_buf_) if (t.value == 1) pushMeasureTrigger(t.t);
        }
        raw.swap(c.raw);
    } else {
        evbuf_.swap(c.ev);
        if (evbuf_.empty()) return;
        std::vector<uint16_t> words;
        enc_.encode(evbuf_.data(), evbuf_.size(), words);
        raw.resize(words.size() * 2);
        std::memcpy(raw.data(), words.data(), raw.size());
    }
    const int64_t t0 = evbuf_.empty() ? dec_.time() : evbuf_.front().t;
    const int64_t t1 = evbuf_.empty() ? dec_.time() : evbuf_.back().t;
    if (t1 > t_now_) t_now_ = t1;
    ev_total_ += evbuf_.size();
    ev_since_ += evbuf_.size();
    int64_t trig = -1;
    handleEvents(evbuf_.data(), evbuf_.size());
    {
        std::lock_guard<std::mutex> lk(rmx_);
        trig = pending_trig_;
        pending_trig_ = -1;
    }

    // pre-trigger ring
    pre_.push_back(RawChunk{std::move(raw), t0, t1, (uint64_t)evbuf_.size()});
    const double keep = std::max(pre_keep_s_, rec_pre_ + 0.1);
    while (pre_.size() > 1 && pre_.front().t1 < t_now_ - (int64_t)(keep * 1e6)) pre_.pop_front();

    std::lock_guard<std::mutex> lk(rmx_);
    if (start_now_) { start_now_ = false; trig = t_now_; }
    if (trig >= 0 && (rec_.state == 1 || rec_.state == 2) && rec_.tTrigger < 0) {
        trigger(trig);
        return;   // the ring (with this chunk) went to the writer
    }
    if (rec_.state == 2 && rec_.tTrigger >= 0) {
        const RawChunk& rc = pre_.back();
        if (rc.t0 <= rec_.tEnd) {
            {
                std::lock_guard<std::mutex> wl(wmx_);
                wq_.push_back(rc.raw);
            }
            wcv_.notify_one();
            rec_.bytes += rc.raw.size();
            rec_.events += rc.nev;
        }
        rec_.seconds = std::max(0.0, (t_now_ - rec_.tStart) * 1e-6);
        if (t1 >= rec_.tEnd) finishRecording();
    }
}

void Engine::handleEvents(const Ev* e, size_t n) {
    std::lock_guard<std::mutex> lk(dmx_);
    const int np = nroi_;
    const bool anyProbe = np > 0;
    const bool actOn = view_mode_.load() == 1;
    // all per-pixel arrays have the sensor size: check it once instead of five comparisons per event
    const size_t cap = std::min({last_t_.size(), last_p_.size(), act_.size(), lut_.size(), sign_.size(), roi_mask_.size()});
    // events arrive in time order, so the time bin changes rarely: cache its ring index (saves two 64-bit divisions per event)
    int64_t rbC = INT64_MIN, bC = INT64_MIN;
    size_t rj = 0, bj = 0;
    bool rOk = false, bOk = false;
    for (size_t i = 0; i < n; ++i) {
        const Ev& ev = e[i];
        if (ev.x >= w_ || ev.y >= h_ || ev.t < 0) continue;   // never index outside the sensor
        const size_t idx = (size_t)ev.y * w_ + ev.x;
        if (idx >= cap) continue;
        last_t_[idx] = ev.t;
        last_p_[idx] = ev.p;
        if (actOn) act_[idx] += 1.f;
        // 1 ms rate bins (+ tap trigger)
        const int64_t rb = ev.t / 1000;
        if (rb != rbC) {
            if (rb > rbin_hi_) advanceRate(rb);
            rbC = rb; rj = (size_t)(rb % nrate_); rOk = rb > rbin_hi_ - nrate_;
        }
        const uint16_t v = lut_[idx];
        if (rOk) {
            rate_all_[rj] += 1.f;
            if (roi_on_ ? roi_mask_[idx] != 0 : (!anyProbe || v != 0)) rate_roi_[rj] += 1.f;
        }
        if (v == 0) continue;
        // probe bins
        const int64_t b = ev.t / kProbeBinUs;
        if (b != bC) {
            if (b > bin_hi_) advanceBins(b);
            bC = b; bj = (size_t)(b % nbins_); bOk = b > bin_hi_ - nbins_;
        }
        if (!bOk) continue;
        const int k = v - 1;
        if (k >= np) continue;
        const float sgn = sign_[idx];
        if (ev.p) r_on_[k][bj] += sgn; else r_off_[k][bj] += sgn;
        r_n_[k][bj] += 1.f;
    }
}

void Engine::advanceBins(int64_t b) {
    // zero the bins that become current (they still hold data from one ring length ago)
    if (bin_hi_ < 0 || b - bin_hi_ >= nbins_) {
        for (auto* r : {&r_on_, &r_off_, &r_n_}) for (auto& v : *r) std::fill(v.begin(), v.end(), 0.f);
    } else {
        for (int64_t i = bin_hi_ + 1; i <= b; ++i) {
            const size_t j = (size_t)(i % nbins_);
            for (size_t k = 0; k < r_on_.size(); ++k) { r_on_[k][j] = 0; r_off_[k][j] = 0; r_n_[k][j] = 0; }
        }
    }
    bin_hi_ = b;
}

void Engine::advanceRate(int64_t rb) {
    const int64_t prev = rbin_hi_;
    if (prev < 0 || rb - prev >= nrate_) {
        std::fill(rate_all_.begin(), rate_all_.end(), 0.f);
        std::fill(rate_roi_.begin(), rate_roi_.end(), 0.f);
    } else {
        for (int64_t i = prev + 1; i <= rb; ++i) { rate_all_[i % nrate_] = 0; rate_roi_[i % nrate_] = 0; }
    }
    rbin_hi_ = rb;
    if (prev >= 0) { checkTrigger(prev); checkMeasureTrigger(prev); }   // bin `prev` (and older) is complete now
}

// tap trigger: the event rate in the ROI over the last 3 ms jumps above factor x the mean of the 0.5 s before
void Engine::checkTrigger(int64_t done) {
    std::lock_guard<std::mutex> lk(rmx_);
    if (rec_.state != 1 || pending_trig_ >= 0) return;
    if ((done - arm_bin_) < 600 || done < 600) return;   // need a baseline after arming
    double cur = 0, base = 0;
    for (int64_t i = done - 2; i <= done; ++i) cur += rate_roi_[i % nrate_];
    for (int64_t i = done - 510; i < done - 10; ++i) base += rate_roi_[i % nrate_];
    cur *= 1000.0 / 3;
    base *= 1000.0 / 500;
    const double floor = 20000;   // ev/s: ignore a jump out of almost nothing
    if (cur > trig_factor_ * std::max(base, floor)) pending_trig_ = (done - 2) * 1000;
}

// split [0, n) over a few threads (the picture is 0.9 M pixels, 60 times a second)
template <class F>
static void parallel_ranges(size_t n, F&& f) {
    static const unsigned nth = std::clamp(std::thread::hardware_concurrency() / 2, 1u, 4u);
    if (nth <= 1 || n < 100000) { f(0, n, 0); return; }
    std::thread th[4];
    const size_t step = (n + nth - 1) / nth;
    for (unsigned k = 1; k < nth; ++k) th[k] = std::thread([&, k] { f(std::min(n, k * step), std::min(n, (k + 1) * step), k); });
    f(0, std::min(n, step), 0);
    for (unsigned k = 1; k < nth; ++k) th[k].join();
}

void Engine::render() {
    hz::ProfScope prof_("engine.render");
    const int mode = view_mode_;
    QImage img(w_, h_, QImage::Format_RGB32);
    const size_t n = (size_t)w_ * h_;
    auto* dst = reinterpret_cast<QRgb*>(img.bits());
    const QRgb bg = qRgb(17, 19, 23);
    if (mode == 0) {
        // last event per pixel, fading with the decay time: ON blue, OFF orange-red
        const double tau = std::max(0.05, decay_ms_.load()) * 1000.0;
        static std::vector<QRgb> lutOn, lutOff;
        const int L = 256;
        if (lutOn.empty()) {
            lutOn.resize(L); lutOff.resize(L);
            for (int k = 0; k < L; ++k) {
                const double a = std::exp(-k * 5.0 / L);
                auto mix = [&](int r, int g, int b) { return qRgb(int(17 + (r - 17) * a), int(19 + (g - 19) * a), int(23 + (b - 23) * a)); };
                lutOn[k] = mix(110, 185, 255);
                lutOff[k] = mix(255, 128, 96);
            }
        }
        const double scale = L / (5.0 * tau);
        const int64_t tn = t_now_, span = (int64_t)(5.0 * tau);
        const int64_t* lt = last_t_.data();
        const uint8_t* lp = last_p_.data();
        parallel_ranges(n, [&](size_t a, size_t b, unsigned) {
            for (size_t i = a; i < b; ++i) {
                const int64_t age = tn - lt[i];
                if (age < 0 || age >= span) { dst[i] = bg; continue; }
                const int k = std::min(L - 1, (int)(age * scale));
                dst[i] = lp[i] ? lutOn[k] : lutOff[k];
            }
        });
    } else {
        // activity: event count per pixel, exponential memory of ~decay x 50, log scale (normalised by the previous
        // frame's maximum, so decay, maximum and colour are one pass)
        const double tau_s = std::max(0.05, decay_ms_.load() * 50.0 / 1000.0);
        const float keep = (float)std::exp(-1.0 / (std::max(5.0, fps_.load()) * tau_s));
        const float inv = 1.f / std::log1p(std::max(1.f, act_max_));
        float mxs[4] = {1.f, 1.f, 1.f, 1.f};
        float* act = act_.data();
        parallel_ranges(n, [&](size_t a, size_t b, unsigned k) {
            float mx = 1.f;
            for (size_t i = a; i < b; ++i) {
                float v = act[i];
                if (v < 0.01f) { act[i] = 0.f; dst[i] = bg; continue; }
                v *= keep;
                act[i] = v;
                mx = std::max(mx, v);
                const float x = std::min(1.f, std::log1p(v) * inv);
                // dark blue -> cyan -> yellow
                const float r = std::clamp(3.f * x - 1.6f, 0.f, 1.f), g = std::clamp(1.6f * x, 0.f, 1.f), bl = std::clamp(0.35f + 1.2f * x - 1.4f * std::max(0.f, x - 0.6f) * 2.f, 0.f, 1.f);
                dst[i] = qRgb(int(17 + r * 238), int(19 + g * 236), int(23 + bl * 232));
            }
            mxs[k] = mx;
        });
        act_max_ = std::max({mxs[0], mxs[1], mxs[2], mxs[3]});
    }
    std::lock_guard<std::mutex> lk(fmx_);
    frame_ = std::move(img);
    ++frame_seq_;
}

// ---- data access -----------------------------------------------------------------------------------------------------
bool Engine::probeWindow(int i, double seconds, ProbeTrace& out, int64_t* tEnd) {
    std::lock_guard<std::mutex> lk(dmx_);
    if (i < 0 || i >= (int)r_on_.size() || bin_hi_ < 0) return false;
    const int64_t end = bin_hi_;   // exclusive: bin_hi_ is still filling
    const int64_t n = std::min<int64_t>((int64_t)(seconds * kProbeFs), nbins_ - 1);
    const int64_t start = end - n;
    out.on.resize(n); out.off.resize(n); out.n.resize(n);
    for (int64_t b = start, k = 0; b < end; ++b, ++k) {
        if (b < 0) { out.on[k] = out.off[k] = out.n[k] = 0; continue; }
        const size_t j = (size_t)(b % nbins_);
        out.on[k] = r_on_[i][j]; out.off[k] = r_off_[i][j]; out.n[k] = r_n_[i][j];
    }
    if (tEnd) *tEnd = end * kProbeBinUs;
    return true;
}

bool Engine::probeRange(int i, int64_t t0, int64_t t1, ProbeTrace& out) {
    std::lock_guard<std::mutex> lk(dmx_);
    if (i < 0 || i >= (int)r_on_.size() || bin_hi_ < 0) return false;
    int64_t b0 = t0 / kProbeBinUs, b1 = t1 / kProbeBinUs;
    b0 = std::max(b0, bin_hi_ - nbins_ + 1);
    b1 = std::min(b1, bin_hi_);
    if (b1 <= b0) return false;
    const int64_t n = b1 - b0;
    out.on.resize(n); out.off.resize(n); out.n.resize(n);
    for (int64_t b = b0, k = 0; b < b1; ++b, ++k) {
        const size_t j = (size_t)(b % nbins_);
        out.on[k] = r_on_[i][j]; out.off[k] = r_off_[i][j]; out.n[k] = r_n_[i][j];
    }
    return true;
}

void Engine::rateWindow(double seconds, std::vector<float>& all, std::vector<float>& roi) {
    std::lock_guard<std::mutex> lk(dmx_);
    const int64_t n = std::min<int64_t>((int64_t)(seconds * 1000), nrate_ - 1);
    all.assign(n, 0.f);
    roi.assign(n, 0.f);
    if (rbin_hi_ < 0) return;
    for (int64_t b = rbin_hi_ - n, k = 0; b < rbin_hi_; ++b, ++k) {
        if (b < 0) continue;
        all[k] = rate_all_[b % nrate_] * 1000.f;
        roi[k] = rate_roi_[b % nrate_] * 1000.f;
    }
}

QVariantMap Engine::stats() {
    QVariantMap m;
    {
        std::lock_guard<std::mutex> lk(qmx_);
        m["queueMB"] = q_bytes_ / 1048576.0;
        m["droppedChunks"] = (qulonglong)dropped_chunks_;
    }
    m["rate"] = ev_rate_;
    m["events"] = (qulonglong)ev_total_;
    m["tNow"] = (double)t_now_ * 1e-6;
    m["badWords"] = (qulonglong)dec_.badWords;
    m["extTriggers"] = (qulonglong)dec_.triggers;
    return m;
}

// ---- recording -------------------------------------------------------------------------------------------------------
bool Engine::record(const QString& path, const std::string& header, double preS, double lenS, bool armed, double factor) {
    std::lock_guard<std::mutex> lk(rmx_);
    if (rec_.state != 0) return false;
    rec_ = RecStatus();
    rec_.path = path;
    header_ = header;
    rec_pre_ = std::clamp(preS, 0.0, 0.9);
    rec_len_ = std::max(0.01, lenS);
    trig_factor_ = std::max(1.2, factor);
    pending_trig_ = -1;
    if (armed) {
        rec_.state = 1;
        arm_bin_ = rbin_hi_;
    } else {
        rec_.state = 2;
        start_now_ = true;
    }
    return true;
}

void Engine::cancel() {
    std::lock_guard<std::mutex> lk(rmx_);
    if (rec_.state == 1) { rec_.state = 0; return; }
    if (rec_.state == 2) {
        if (rec_.tTrigger < 0) { rec_.state = 0; start_now_ = false; return; }
        finishRecording();
    }
}

RecStatus Engine::recStatus() {
    std::lock_guard<std::mutex> lk(rmx_);
    RecStatus r = rec_;
    rec_.finished = false;
    rec_.error.clear();
    return r;
}

// rmx_ held
void Engine::trigger(int64_t t) {
    rec_.tTrigger = t;
    rec_.tStart = t - (int64_t)(rec_pre_ * 1e6);
    rec_.tEnd = t + (int64_t)(rec_len_ * 1e6);
    rec_.state = 2;
    startWriting(rec_.tStart);
}

// rmx_ held
void Engine::startWriting(int64_t tFrom) {
    std::lock_guard<std::mutex> wl(wmx_);
#ifdef _WIN32
    wf_ = _wfopen(rec_.path.toStdWString().c_str(), L"wb");
#else
    wf_ = fopen(rec_.path.toUtf8().constData(), "wb");
#endif
    if (!wf_) {
        rec_.error = "Cannot write " + rec_.path;
        rec_.state = 0;
        return;
    }
    setvbuf(wf_, nullptr, _IOFBF, 8 << 20);
    fwrite(header_.data(), 1, header_.size(), wf_);
    for (const RawChunk& rc : pre_) {
        if (rc.t1 < tFrom) continue;
        if (rec_.tFirst < 0) rec_.tFirst = rc.t0;
        wq_.push_back(rc.raw);
        rec_.bytes += rc.raw.size();
        rec_.events += rc.nev;
    }
    rec_.seconds = (t_now_ - tFrom) * 1e-6;
    wcv_.notify_one();
    if (t_now_ >= rec_.tEnd) { w_close_ = true; rec_.state = 3; }
}

// rmx_ held
void Engine::finishRecording() {
    rec_.state = 3;
    {
        std::lock_guard<std::mutex> wl(wmx_);
        w_close_ = true;
    }
    wcv_.notify_one();
}

void Engine::writerLoop() {
    for (;;) {
        std::vector<uint8_t> buf;
        bool close = false;
        {
            std::unique_lock<std::mutex> lk(wmx_);
            wcv_.wait(lk, [&] { return quit_ || !wq_.empty() || w_close_; });
            if (!wq_.empty()) { buf.swap(wq_.front()); wq_.pop_front(); }
            else if (w_close_) { close = true; w_close_ = false; }
            else if (quit_) return;
        }
        if (!buf.empty()) {
            FILE* f;
            {
                std::lock_guard<std::mutex> lk(wmx_);
                f = wf_;
            }
            if (f) fwrite(buf.data(), 1, buf.size(), f);   // only this thread closes the file
        }
        if (close) {
            bool ok = true;
            {
                std::lock_guard<std::mutex> lk(wmx_);
                if (wf_) { ok = fflush(wf_) == 0; ok = fclose(wf_) == 0 && ok; wf_ = nullptr; }
            }
            std::lock_guard<std::mutex> lk(rmx_);
            rec_.state = 0;
            rec_.finished = true;
            if (!ok) rec_.error = "Writing " + rec_.path + " failed (disk full?)";
        }
    }
}

}  // namespace evs

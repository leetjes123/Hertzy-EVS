#include "clip.hpp"
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <map>
#include <numeric>
#include <thread>
#include "pixelmodel.hpp"
#include "pocketfft_hdronly.h"

namespace evs {

// RAW header: "% key value" lines up to "% end"; returns the data offset or -1
static qint64 header_of(QFile& f, int& W, int& H) {
    qint64 pos = 0;
    bool any = false;
    while (!f.atEnd()) {
        char c = 0;
        if (!f.peek(&c, 1) || c != '%') break;
        const QByteArray line = f.readLine();
        pos += line.size();
        any = true;
        const QByteArray body = line.mid(2).trimmed();
        if (body == "end") return pos;
        if (body.startsWith("geometry ")) {
            const QList<QByteArray> p = body.mid(9).split('x');
            if (p.size() == 2) { W = p[0].toInt(); H = p[1].toInt(); }
        }
    }
    return any ? pos : (f.seek(0), 0);
}

template <class OnEvents>
static bool decode_file(const QString& path, int& W, int& H, QString* err, const Progress& prog, const std::atomic<bool>* cancel,
                        std::vector<TrigEv>* trig, OnEvents&& onEvents) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { if (err) *err = "Cannot open " + path; return false; }
    W = 1280; H = 720;
    const qint64 off = header_of(f, W, H);
    if (W <= 0 || H <= 0 || W > 2048 || H > 2048) { if (err) *err = "Unsupported sensor size in " + path; return false; }
    f.seek(off);
    const qint64 total = std::max<qint64>(1, f.size() - off);
    Evt3Decoder dec;
    dec.width = W;
    dec.height = H;
    dec.trigOut = trig;
    std::vector<uint8_t> buf(8 << 20);
    std::vector<Ev> ev;
    qint64 done = 0;
    while (true) {
        if (cancel && cancel->load()) { if (err) *err = "cancelled"; return false; }
        const qint64 got = f.read(reinterpret_cast<char*>(buf.data()), (qint64)buf.size());
        if (got <= 0) break;
        ev.clear();
        dec.decodeBytes(buf.data(), (size_t)got, ev);
        if (!onEvents(ev)) return false;
        done += got;
        if (prog) prog(double(done) / total);
    }
    return true;
}

bool scan_raw(const QString& path, ScanOut& out, QString* err, const Progress& prog, const std::atomic<bool>* cancel) {
    out = ScanOut();
    int64_t t0 = -1, tl = 0;
    std::vector<TrigEv> trig;
    bool sized = false;
    const bool ok = decode_file(path, out.W, out.H, err, prog, cancel, &trig, [&](const std::vector<Ev>& ev) {
        if (!sized) { out.count.assign((size_t)out.W * out.H, 0.f); sized = true; }
        for (const Ev& e : ev) {
            if (t0 < 0) t0 = e.t;
            if (e.t > tl) tl = e.t;
            out.count[(size_t)e.y * out.W + e.x] += 1.f;
            out.on += e.p;
            const int64_t b = (e.t - t0) / 1000;
            if (b < 0) continue;
            if (b >= (int64_t)out.rate.size()) {
                if (b > 3600 * 1000) continue;   // garbage time stamp
                out.rate.resize(b + 1, 0.f);
            }
            out.rate[b] += 1.f;
        }
        out.events += ev.size();
        return true;
    });
    if (!ok) return false;
    if (!sized) out.count.assign((size_t)out.W * out.H, 0.f);
    out.t0 = std::max<int64_t>(t0, 0);
    out.duration = t0 >= 0 ? (tl - t0) * 1e-6 : 0;
    for (TrigEv& tr : trig) { tr.t -= out.t0; out.triggers.push_back(tr); }
    if (out.events == 0 && err) *err = "No events in " + path;
    return out.events > 0;
}

// ---- clip -------------------------------------------------------------------------------------------------------------
bool Clip::load(const QString& rawPath, QString* err, const Progress& prog, const std::atomic<bool>* cancel) {
    path = rawPath;
    name = QFileInfo(rawPath).completeBaseName();
    meta = {};
    QFile jf(rawPath.left(rawPath.size() - 4) + ".json");
    if (jf.open(QIODevice::ReadOnly)) meta = QJsonDocument::fromJson(jf.readAll()).object();
    const QJsonObject b = meta["biases"].toObject();
    thetaOn = b.contains("bias_diff_on") ? theta_on(b["bias_diff_on"].toDouble()) : 0.2;
    thetaOff = b.contains("bias_diff_off") ? theta_off(b["bias_diff_off"].toDouble()) : 0.2;
    xyp.clear(); t.clear(); triggers.clear();
    {
        const qint64 sz = QFileInfo(rawPath).size();
        const size_t guess = (size_t)std::min<qint64>(sz / 2, (qint64)kMaxEvents);   // ~1 event per word for EVT3
        xyp.reserve(guess); t.reserve(guess);
    }
    int64_t t0 = -1;
    bool sorted = true;
    uint32_t last = 0;
    std::vector<TrigEv> trig;
    const bool ok = decode_file(rawPath, W, H, err, prog, cancel, &trig, [&](const std::vector<Ev>& ev) {
        if (xyp.size() + ev.size() > kMaxEvents) {
            if (err) *err = QString("The recording has more than %1 M events: too big to analyse in memory").arg(kMaxEvents / 1000000);
            return false;
        }
        for (const Ev& e : ev) {
            if (t0 < 0) t0 = e.t;
            const int64_t dt = e.t - t0;
            if (dt < 0 || dt > 0xFFFFFFF0LL) continue;
            const uint32_t tt = (uint32_t)dt;
            if (tt < last) sorted = false;
            last = tt;
            xyp.push_back(uint32_t(e.x) | uint32_t(e.y) << 11 | uint32_t(e.p) << 22);
            t.push_back(tt);
        }
        return true;
    });
    if (!ok) { xyp.clear(); t.clear(); xyp.shrink_to_fit(); t.shrink_to_fit(); return false; }
    if (t.empty()) { if (err) *err = "No events in " + rawPath; return false; }
    if (!sorted) {   // rare: the stream stepped back a little; keep the time order the index needs
        std::vector<uint32_t> o(t.size());
        std::iota(o.begin(), o.end(), 0u);
        std::stable_sort(o.begin(), o.end(), [&](uint32_t a, uint32_t c) { return t[a] < t[c]; });
        std::vector<uint32_t> a(t.size()), c(t.size());
        for (size_t i = 0; i < o.size(); ++i) { a[i] = xyp[o[i]]; c[i] = t[o[i]]; }
        xyp.swap(a);
        t.swap(c);
    }
    xyp.shrink_to_fit();
    t.shrink_to_fit();
    t0abs = t0;
    duration = t.back() * 1e-6;
    for (TrigEv& tr : trig) { tr.t -= t0; triggers.push_back(tr); }
    // 1 ms index and rates
    const size_t nms = (size_t)(t.back() / 1000) + 1;
    msIndex.assign(nms + 1, 0);
    rateAll.assign(nms, 0.f);
    rateOn.assign(nms, 0.f);
    for (size_t i = 0; i < t.size(); ++i) {
        const size_t b = t[i] / 1000;
        rateAll[b] += 1.f;
        rateOn[b] += float(ep(xyp[i]));
    }
    size_t acc = 0;
    for (size_t b = 0; b < nms; ++b) { msIndex[b] = (uint32_t)acc; acc += (size_t)rateAll[b]; }
    msIndex[nms] = (uint32_t)acc;
    return true;
}

size_t Clip::at(double s) const {
    if (s <= 0) return 0;
    const int64_t us = (int64_t)std::llround(s * 1e6);
    if (us > (int64_t)t.back()) return t.size();
    const size_t b = (size_t)(us / 1000);
    size_t i = b < msIndex.size() ? msIndex[b] : t.size();
    const size_t e = b + 1 < msIndex.size() ? msIndex[b + 1] : t.size();
    while (i < e && (int64_t)t[i] < us) ++i;
    return i;
}

void Clip::countMaps(double t0, double t1, std::vector<float>& on, std::vector<float>& off) const {
    on.assign((size_t)W * H, 0.f);
    off.assign((size_t)W * H, 0.f);
    const size_t a = at(t0), b = at(t1);
    for (size_t i = a; i < b; ++i) {
        const uint32_t v = xyp[i];
        const size_t idx = (size_t)ey(v) * W + ex(v);
        if (idx >= on.size()) continue;
        (ep(v) ? on : off)[idx] += 1.f;
    }
}

long long Clip::roiSignal(const RoiPixels& px, SignalKind kind, bool sided, double t0, double t1, double fs, std::vector<double>& out) const {
    const int nb = std::max(0, (int)std::floor((t1 - t0) * fs + 1e-9));
    out.assign(nb, 0.0);
    if (nb == 0 || px.idx.empty()) return 0;
    std::vector<int8_t> w((size_t)W * H, 0);
    for (size_t i = 0; i < px.idx.size(); ++i) if (px.idx[i] < w.size()) w[px.idx[i]] = sided ? px.sign[i] : 1;
    const size_t a = at(t0), b = at(t1);
    const double bin = 1e6 / fs, start = t0 * 1e6;
    const SignalKind k = kind == SignalKind::Auto ? SignalKind::OnOff : kind;
    const double won = thetaOn, woff = thetaOff;
    long long n = 0;
    for (size_t i = a; i < b; ++i) {
        const uint32_t v = xyp[i];
        const size_t idx = (size_t)ey(v) * W + ex(v);
        const int8_t s = idx < w.size() ? w[idx] : 0;
        if (!s) continue;
        int j = (int)((t[i] - start) / bin);
        if (j < 0) continue;
        if (j >= nb) j = nb - 1;
        ++n;
        const bool on = ep(v) != 0;
        switch (k) {
        case SignalKind::OnOff: out[j] += s * (on ? won : -woff); break;
        case SignalKind::On: if (on) out[j] += 1; break;
        case SignalKind::Off: if (!on) out[j] += 1; break;
        default: out[j] += 1; break;
        }
    }
    return n;
}

std::vector<float> Clip::roiRate(const RoiPixels* px) const {
    if (!px) return rateAll;
    std::vector<uint8_t> m((size_t)W * H, 0);
    for (uint32_t i : px->idx) if (i < m.size()) m[i] = 1;
    std::vector<float> r(rateAll.size(), 0.f);
    for (size_t i = 0; i < t.size(); ++i) {
        const uint32_t v = xyp[i];
        const size_t idx = (size_t)ey(v) * W + ex(v);
        if (idx < m.size() && m[idx]) r[t[i] / 1000] += 1.f;
    }
    return r;
}

// ---- per-pixel spectra ------------------------------------------------------------------------------------------------
PixelSpectra pixel_spectra(const Clip& c, double t0, double t1, double fs, double toneLo, double toneHi, double bandLo, double bandHi,
                           int maxPix, const std::atomic<bool>* cancel) {
    PixelSpectra ps;
    ps.W = c.W;
    ps.H = c.H;
    const size_t np = (size_t)c.W * c.H;
    ps.freq.assign(np, NAN);
    ps.amp.assign(np, 0.f);
    ps.snr.assign(np, 0.f);
    ps.energy.assign(np, 0.f);
    ps.fLo = toneLo;
    ps.fHi = toneHi;
    const int nb = (int)std::floor((t1 - t0) * fs);
    if (nb < 32) return ps;
    // the most active pixels
    std::vector<float> on, off;
    c.countMaps(t0, t1, on, off);
    std::vector<uint32_t> cand;
    for (size_t i = 0; i < np; ++i) if (on[i] + off[i] >= 4) cand.push_back((uint32_t)i);
    if ((int)cand.size() > maxPix) {
        std::nth_element(cand.begin(), cand.begin() + maxPix, cand.end(), [&](uint32_t a, uint32_t b) { return on[a] + off[a] > on[b] + off[b]; });
        cand.resize(maxPix);
    }
    ps.pixels = (int)cand.size();
    if (cand.empty()) return ps;
    const size_t a = c.at(t0), b = c.at(t1);
    const double bin = 1e6 / fs, start = t0 * 1e6;
    const int N = std::max(nb, (int)std::ceil(fs / 2.0));   // zero-pad to <= 2 Hz bins
    const double df = fs / N;
    const int kLo = std::max(1, (int)std::ceil(toneLo / df)), kHi = std::min(N / 2 - 1, (int)std::floor(toneHi / df));
    const int eLo = std::max(1, (int)std::ceil(bandLo / df)), eHi = std::min(N / 2 - 1, (int)std::floor(bandHi / df));
    if (kHi <= kLo) return ps;
    std::vector<double> win(nb);
    double wsum = 0;
    for (int i = 0; i < nb; ++i) { win[i] = 0.5 - 0.5 * std::cos(2 * M_PI * i / std::max(1, nb - 1)); wsum += win[i]; }
    const size_t batch = 4096;
    std::vector<int32_t> slot(np, -1);
    const unsigned nth = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
    for (size_t b0 = 0; b0 < cand.size(); b0 += batch) {
        if (cancel && cancel->load()) return ps;
        const size_t n = std::min(batch, cand.size() - b0);
        std::vector<float> S(n * (size_t)nb, 0.f);
        for (size_t k = 0; k < n; ++k) slot[cand[b0 + k]] = (int32_t)k;
        for (size_t i = a; i < b; ++i) {
            const uint32_t v = c.xyp[i];
            const size_t idx = (size_t)Clip::ey(v) * c.W + Clip::ex(v);
            const int32_t s = slot[idx];
            if (s < 0) continue;
            int j = (int)((c.t[i] - start) / bin);
            if (j < 0 || j >= nb) continue;
            S[(size_t)s * nb + j] += Clip::ep(v) ? 1.f : -1.f;
        }
        for (size_t k = 0; k < n; ++k) slot[cand[b0 + k]] = -1;
        std::vector<std::thread> th;
        for (unsigned w = 0; w < nth; ++w)
            th.emplace_back([&, w] {
                std::vector<double> x(N);
                std::vector<std::complex<double>> X(N / 2 + 1);
                std::vector<double> mag(N / 2 + 1), band;
                for (size_t k = w; k < n; k += nth) {
                    const float* s = &S[k * nb];
                    double mean = 0;
                    for (int i = 0; i < nb; ++i) mean += s[i];
                    mean /= nb;
                    std::fill(x.begin(), x.end(), 0.0);
                    for (int i = 0; i < nb; ++i) x[i] = (s[i] - mean) * win[i];
                    pocketfft::r2c<double>({(size_t)N}, {sizeof(double)}, {sizeof(std::complex<double>)}, 0, pocketfft::FORWARD, x.data(), X.data(), 1.0);
                    for (int i = 0; i <= N / 2; ++i) mag[i] = std::abs(X[i]) * 2 / wsum;
                    int best = kLo;
                    band.assign(mag.begin() + kLo, mag.begin() + kHi + 1);
                    for (int i = kLo; i <= kHi; ++i) if (mag[i] > mag[best]) best = i;
                    std::nth_element(band.begin(), band.begin() + band.size() / 2, band.end());
                    const double med = band[band.size() / 2];
                    double e = 0;
                    for (int i = eLo; i <= eHi; ++i) e += mag[i] * mag[i];
                    const uint32_t p = cand[b0 + k];
                    ps.freq[p] = float(best * df);
                    ps.amp[p] = float(mag[best]);
                    ps.snr[p] = float(med > 0 ? mag[best] / med : 0);
                    ps.energy[p] = float(e);
                }
            });
        for (auto& t : th) t.join();
    }
    return ps;
}

// ---- colour maps ------------------------------------------------------------------------------------------------------
static QRgb lerp_stops(const float (*s)[3], int n, float t) {
    t = std::clamp(t, 0.f, 1.f) * (n - 1);
    const int i = std::min(n - 2, (int)t);
    const float f = t - i;
    return qRgb(int(255 * (s[i][0] + f * (s[i + 1][0] - s[i][0]))), int(255 * (s[i][1] + f * (s[i + 1][1] - s[i][1]))),
                int(255 * (s[i][2] + f * (s[i + 1][2] - s[i][2]))));
}

QRgb inferno(float t) {
    static const float s[][3] = {{0.07f, 0.075f, 0.09f}, {0.16f, 0.04f, 0.33f}, {0.42f, 0.07f, 0.43f}, {0.69f, 0.16f, 0.33f},
                                 {0.90f, 0.36f, 0.17f}, {0.98f, 0.64f, 0.04f}, {0.99f, 1.0f, 0.64f}};
    return lerp_stops(s, 7, t);
}

QRgb turbo(float t) {
    static const float s[][3] = {{0.19f, 0.07f, 0.23f}, {0.27f, 0.42f, 0.88f}, {0.16f, 0.74f, 0.88f}, {0.27f, 0.97f, 0.49f},
                                 {0.73f, 0.96f, 0.21f}, {0.98f, 0.73f, 0.21f}, {0.93f, 0.36f, 0.10f}, {0.64f, 0.07f, 0.01f}};
    return lerp_stops(s, 8, t);
}

QImage render_log(const std::vector<float>& v, int W, int H, float* vmax) {
    QImage img(W, H, QImage::Format_RGB32);
    float mx = 0;
    for (float x : v) mx = std::max(mx, x);
    if (vmax) *vmax = mx;
    const float lm = std::log1p(std::max(mx, 1.f));
    const QRgb bg = qRgb(17, 19, 23);
    for (int y = 0; y < H; ++y) {
        QRgb* row = reinterpret_cast<QRgb*>(img.scanLine(y));
        for (int x = 0; x < W; ++x) {
            const float a = v[(size_t)y * W + x];
            row[x] = a > 0 ? inferno(0.08f + 0.92f * std::log1p(a) / lm) : bg;
        }
    }
    return img;
}

QImage render_polarity(const std::vector<float>& on, const std::vector<float>& off, int W, int H) {
    QImage img(W, H, QImage::Format_RGB32);
    float mx = 1;
    for (size_t i = 0; i < on.size(); ++i) mx = std::max(mx, on[i] + off[i]);
    const float lm = std::log1p(mx);
    for (int y = 0; y < H; ++y) {
        QRgb* row = reinterpret_cast<QRgb*>(img.scanLine(y));
        for (int x = 0; x < W; ++x) {
            const size_t i = (size_t)y * W + x;
            const float n = on[i] + off[i];
            if (n <= 0) { row[x] = qRgb(17, 19, 23); continue; }
            const float a = 0.25f + 0.75f * std::log1p(n) / lm, f = on[i] / n;   // f = ON fraction
            // ON = blue (110,185,255), OFF = orange (255,128,96), mixed = grey
            const float r = 255 * (1 - f) + 110 * f, g = 128 * (1 - f) + 185 * f, b = 96 * (1 - f) + 255 * f;
            row[x] = qRgb(int(17 + (r - 17) * a), int(19 + (g - 19) * a), int(23 + (b - 23) * a));
        }
    }
    return img;
}

QImage render_freq(const PixelSpectra& ps, double snrMin) {
    QImage img(ps.W, ps.H, QImage::Format_RGB32);
    // brightness: amplitude relative to the 98th percentile of the pixels that pass the SNR limit (one hot pixel can't darken all)
    std::vector<float> a;
    for (size_t i = 0; i < ps.amp.size(); ++i) if (ps.snr[i] >= snrMin && ps.amp[i] > 0) a.push_back(ps.amp[i]);
    float hi = 1e-9f;
    if (!a.empty()) { const size_t k = (size_t)(a.size() * 0.98); std::nth_element(a.begin(), a.begin() + std::min(k, a.size() - 1), a.end()); hi = std::max(hi, a[std::min(k, a.size() - 1)]); }
    const QRgb bg = qRgb(17, 19, 23);
    for (int y = 0; y < ps.H; ++y) {
        QRgb* row = reinterpret_cast<QRgb*>(img.scanLine(y));
        for (int x = 0; x < ps.W; ++x) {
            const size_t i = (size_t)y * ps.W + x;
            if (!(ps.freq[i] == ps.freq[i])) { row[x] = bg; continue; }
            const QRgb c = turbo(float((ps.freq[i] - ps.fLo) / std::max(1e-9, ps.fHi - ps.fLo)));
            const float a2 = ps.snr[i] >= snrMin ? 0.45f + 0.55f * std::sqrt(std::min(1.f, ps.amp[i] / hi)) : 0.16f;
            row[x] = qRgb(int(17 + (qRed(c) - 17) * a2), int(19 + (qGreen(c) - 19) * a2), int(23 + (qBlue(c) - 23) * a2));
        }
    }
    return img;
}

QImage render_energy(const PixelSpectra& ps) {
    std::vector<float> e(ps.energy.size());
    float mx = 0;
    for (float v : ps.energy) mx = std::max(mx, v);
    const float s = mx > 0 ? 1e4f / mx : 1.f;
    for (size_t i = 0; i < e.size(); ++i) e[i] = ps.energy[i] * s;
    return render_log(e, ps.W, ps.H);
}

QImage thumbnail(const std::vector<float>& count, int W, int H, int tw) {
    const int f = std::max(1, W / tw), th = H / f, tw2 = W / f;
    std::vector<float> s((size_t)tw2 * th, 0.f);
    for (int y = 0; y < th * f; ++y)
        for (int x = 0; x < tw2 * f; ++x) s[(size_t)(y / f) * tw2 + x / f] += count[(size_t)y * W + x];
    // robust scale: the 99.7th percentile of the non-empty pixels is "full"; one hot pixel can't wash out the picture
    std::vector<float> nz;
    for (float v : s) if (v > 0) nz.push_back(v);
    float hi = 1;
    if (!nz.empty()) {
        const size_t k = std::min(nz.size() - 1, (size_t)(nz.size() * 0.997));
        std::nth_element(nz.begin(), nz.begin() + k, nz.end());
        hi = std::max(1.f, nz[k]);
    }
    for (float& v : s) v = std::min(v, hi);
    return render_log(s, tw2, th);
}

}  // namespace evs

#include "measure.hpp"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QTextStream>
#include <algorithm>
#include <cmath>
#include <complex>
#include <numeric>
#include <thread>
#include "dsp.hpp"
#include "pocketfft_hdronly.h"
#include "signal.hpp"

namespace evs {

// ---- settings -----------------------------------------------------------------------------------------------------------
QJsonObject MeasureSettings::toJson() const {
    return QJsonObject{{"fs", fs}, {"signal", signal}, {"window", window}, {"band_hz", QJsonArray{bandLo, bandHi}},
                       {"tone_hz", QJsonArray{toneLo, toneHi}}, {"df_hz", df}, {"snr_min", snrMin}, {"fmax_hz", fmax},
                       {"common_mode", commonMode}, {"win_mode", winMode}, {"t0_s", t0}, {"t1_s", t1}, {"skip_s", skip},
                       {"len_s", len}, {"onset", onset}, {"sg_win_ms", sgWinMs}, {"sg_hop_ms", sgHopMs},
                       {"rpm", QJsonObject{{"on", rpmOn}, {"marks", rpmMarks}, {"range_rpm", QJsonArray{rpmLo, rpmHi}}, {"track_win_s", rpmWin}}}};
}

MeasureSettings MeasureSettings::fromJson(const QJsonObject& o) {
    MeasureSettings s;
    s.fs = o["fs"].toDouble(s.fs);
    s.signal = o["signal"].toString(s.signal);
    s.window = o["window"].toString(s.window);
    const QJsonArray b = o["band_hz"].toArray(), t = o["tone_hz"].toArray();
    if (b.size() == 2) { s.bandLo = b[0].toDouble(); s.bandHi = b[1].toDouble(); }
    if (t.size() == 2) { s.toneLo = t[0].toDouble(); s.toneHi = t[1].toDouble(); }
    s.df = o["df_hz"].toDouble(s.df);
    s.snrMin = o["snr_min"].toDouble(s.snrMin);
    s.fmax = o["fmax_hz"].toDouble(s.fmax);
    s.commonMode = o["common_mode"].toBool(s.commonMode);
    s.winMode = o["win_mode"].toString(s.winMode);
    s.t0 = o["t0_s"].toDouble(s.t0);
    s.t1 = o["t1_s"].toDouble(s.t1);
    s.skip = o["skip_s"].toDouble(s.skip);
    s.len = o["len_s"].toDouble(s.len);
    s.onset = o["onset"].toInt(s.onset);
    s.sgWinMs = o["sg_win_ms"].toDouble(s.sgWinMs);
    s.sgHopMs = o["sg_hop_ms"].toDouble(s.sgHopMs);
    const QJsonObject rp = o["rpm"].toObject();
    s.rpmOn = rp["on"].toBool(s.rpmOn);
    s.rpmMarks = rp["marks"].toInt(s.rpmMarks);
    const QJsonArray rr = rp["range_rpm"].toArray();
    if (rr.size() == 2) { s.rpmLo = rr[0].toDouble(); s.rpmHi = rr[1].toDouble(); }
    s.rpmWin = rp["track_win_s"].toDouble(s.rpmWin);
    return s;
}

// ---- onsets -------------------------------------------------------------------------------------------------------------
static double median_of(std::vector<double> v) {
    if (v.empty()) return 0;
    const size_t h = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + h, v.end());
    return v[h];
}

std::vector<Onset> find_onsets(const Clip& c, double jump, double minGap, const std::vector<float>* rate) {
    const std::vector<float>& r0 = rate ? *rate : c.rateAll;
    const int n = (int)r0.size();
    std::vector<double> r(n, 0.0);   // 3 ms moving average (np.convolve "same")
    for (int i = 0; i < n; ++i) {
        double s = 0;
        for (int k = -1; k <= 1; ++k) if (i + k >= 0 && i + k < n) s += r0[i + k];
        r[i] = s / 3;
    }
    std::vector<Onset> out;
    auto medRange = [&](int a, int b) { std::vector<double> v; for (int i = std::max(0, a); i < std::min(b, n); ++i) v.push_back(r[i]); return median_of(v); };
    // onset near bin i: first bin above pre + 50 % of (peak in the next 20 bins - pre)
    auto onsetNear = [&](int i, double& pre) {
        pre = medRange(i - 50, std::max(i - 2, 1));
        double pk = 0;
        for (int k = i; k < std::min(n, i + 20); ++k) pk = std::max(pk, r[k]);
        const double lvl = pre + 0.5 * (pk - pre);
        for (int k = std::max(0, i - 5); k < std::min(n, i + 20); ++k) if (r[k] > lvl) return k;
        return std::max(0, i);
    };
    bool anyTrig = false;
    for (const TrigEv& t : c.triggers) if (t.value == 1) { anyTrig = true; break; }
    if (anyTrig) {
        double lastT = -1e9;
        for (const TrigEv& t : c.triggers) {
            if (t.value != 1) continue;
            const int i = (int)(t.t / 1000);
            if (i >= n || t.t * 1e-6 - lastT < minGap * 0.5) continue;
            lastT = t.t * 1e-6;
            int best = i;
            double rise = -1e18;
            for (int k = i + 1; k < std::min(n, i + 80); ++k) if (r[k] - r[k - 1] > rise) { rise = r[k] - r[k - 1]; best = k; }
            double pre = 0;
            const int on = onsetNear(best, pre);
            out.push_back({on * 1e-3, pre, "trigger"});
        }
        return out;
    }
    int i = 4;
    const int gap = std::max(5, (int)(minGap * 1000));
    while (i < n - 20) {
        const double base = medRange(i - 50, i - 2);
        if (!(r[i] > jump * std::max(base, 1.0) && r[i] > r[i - 1])) { ++i; continue; }
        const int g1 = std::min(i + gap, n - 20);
        int j = i;
        double best = -1e18;
        for (int k = i; k < g1; ++k) { const double d = r[k] - r[std::max(0, k - 3)]; if (d > best) { best = d; j = k; } }
        double pk = 0;
        for (int k = j; k < std::min(n, j + 10); ++k) pk = std::max(pk, r[k]);
        int on = j;
        for (int k = std::max(0, j - 10); k < std::min(n, j + 10); ++k) if (r[k] > base + 0.5 * (pk - base)) { on = k; break; }
        out.push_back({on * 1e-3, base, "event rate"});
        i = std::max(g1, i + 1);
    }
    return out;
}

bool measure_window(const Clip& c, const MeasureSettings& s, const std::vector<Onset>& onsets, double& t0, double& t1, QString* why) {
    if (s.winMode == "onset") {
        if (s.onset < 0 || s.onset >= (int)onsets.size()) { if (why) *why = onsets.empty() ? "No onset (tap / pluck) found in this clip" : QString("Onset %1 does not exist (%2 found)").arg(s.onset + 1).arg(onsets.size()); return false; }
        t0 = onsets[s.onset].t + s.skip;
        t1 = t0 + s.len;
    } else {
        t0 = std::max(0.0, s.t0);
        t1 = s.t1 > s.t0 ? s.t1 : c.duration;
    }
    t1 = std::min(t1, c.duration);
    if (t1 - t0 < 32.0 / s.fs) { if (why) *why = "The time window is too short (or past the end of the clip)"; return false; }
    return true;
}

// ---- spectra --------------------------------------------------------------------------------------------------------------
void spectrum_of(const std::vector<double>& x, const MeasureSettings& s, std::vector<double>& f, std::vector<double>& amp) {
    f.clear();
    amp.clear();
    const int n = (int)x.size();
    if (n < 16) return;
    const int N = std::max(n, (int)std::ceil(s.fs / std::max(0.01, s.df)));
    std::vector<double> buf(N, 0.0), w(n, 1.0);
    if (s.window != "rect")
        for (int i = 0; i < n; ++i) w[i] = 0.5 - 0.5 * std::cos(2 * M_PI * i / std::max(1, n - 1));
    const double mean = std::accumulate(x.begin(), x.end(), 0.0) / n, wsum = std::accumulate(w.begin(), w.end(), 0.0);
    for (int i = 0; i < n; ++i) buf[i] = (x[i] - mean) * w[i];
    std::vector<std::complex<double>> X(N / 2 + 1);
    pocketfft::r2c<double>({(size_t)N}, {sizeof(double)}, {sizeof(std::complex<double>)}, 0, pocketfft::FORWARD, buf.data(), X.data(), 1.0);
    const double df = s.fs / N;
    for (int k = 0; k <= N / 2 && k * df <= s.fmax; ++k) { f.push_back(k * df); amp.push_back(std::abs(X[k]) * 2 / wsum); }
}

long long roi_series(const Clip& c, const Roi& r, const RoiPixels& px, const MeasureSettings& s, double t0, double t1, std::vector<double>& out) {
    if (s.signal != "displacement") return c.roiSignal(px, r.signal, r.sided(), t0, t1, s.fs, out);
    // displacement: integrate over a padded span (filter edge effects stay outside the window), detrend, band-pass, cut
    const double pad = 0.05;
    const double a = std::max(0.0, t0 - pad), b = std::min(c.duration, t1 + pad);
    std::vector<double> v;
    const long long n = c.roiSignal(px, r.signal, r.sided(), a, b, s.fs, v);
    double acc = 0;
    for (double& x : v) { acc += x; x = acc; }
    const size_t m = v.size();
    if (m >= 64) {
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        for (size_t i = 0; i < m; ++i) { sx += i; sy += v[i]; sxx += double(i) * i; sxy += i * v[i]; }
        const double den = m * sxx - sx * sx, k = den != 0 ? (m * sxy - sx * sy) / den : 0, c0 = (sy - k * sx) / m;
        for (size_t i = 0; i < m; ++i) v[i] -= c0 + k * i;
        const auto sos = hz::dsp::butter(4, s.bandLo > 0 ? s.bandLo : 0, s.bandHi > 0 && s.bandHi < s.fs / 2 ? s.bandHi : 0, s.fs);
        if (!sos.empty()) v = hz::dsp::sosfiltfilt(sos, v);
    }
    const int i0 = (int)std::lround((t0 - a) * s.fs), nb = (int)std::floor((t1 - t0) * s.fs + 1e-9);
    out.assign(nb, 0.0);
    for (int i = 0; i < nb && i0 + i < (int)v.size(); ++i) out[i] = v[i0 + i];
    return n;
}

static double amp_at(const RoiResult& r, double f) {
    if (r.f.size() < 2) return 0;
    const double df = r.f[1] - r.f[0];
    const size_t k = (size_t)std::lround(f / df);
    return k < r.amp.size() ? r.amp[k] : 0;
}

void pick_tones(std::vector<RoiResult>& rr, const MeasureSettings& s, std::vector<double>* commonOut) {
    for (RoiResult& r : rr) {
        r.peaks.clear();
        r.tone = {};
        r.ok = false;
        const int n = (int)r.f.size();
        if (n < 4) continue;
        const double df = r.f[1] - r.f[0];
        std::vector<double> band;
        for (int k = 1; k < n; ++k) if (r.f[k] >= s.bandLo && r.f[k] <= s.bandHi) band.push_back(r.amp[k]);
        r.median = median_of(band);
        if (r.median <= 0) continue;
        std::vector<TonePeak> pk;
        for (int k = 1; k + 1 < n; ++k) {
            if (r.f[k] < s.toneLo || r.f[k] > s.toneHi) continue;
            if (!(r.amp[k] > r.amp[k - 1] && r.amp[k] >= r.amp[k + 1])) continue;
            if (r.amp[k] < 3 * r.median) continue;
            const double a = r.amp[k - 1], b = r.amp[k], c = r.amp[k + 1], den = a - 2 * b + c;
            const double dl = den != 0 ? std::clamp(0.5 * (a - c) / den, -0.5, 0.5) : 0.0;
            pk.push_back({r.f[k] + dl * df, (b - 0.25 * (a - c) * dl) / r.median, b - 0.25 * (a - c) * dl});
        }
        std::sort(pk.begin(), pk.end(), [](const TonePeak& a, const TonePeak& b) { return a.snr > b.snr; });
        // keep peaks at least 1 Hz apart (strongest first)
        for (const TonePeak& p : pk) {
            bool near = false;
            for (const TonePeak& q : r.peaks) if (std::fabs(q.f - p.f) < 1.0) { near = true; break; }
            if (!near) r.peaks.push_back(p);
            if (r.peaks.size() >= 8) break;
        }
    }
    // common-mode tones: on >= max(3, 65 %) of the ROIs, the strongest peak of < 1/3 of them and no ROI dominating
    std::vector<double> common;
    const int ns = (int)rr.size();
    if (s.commonMode && ns >= 3) {
        std::vector<double> cand;
        for (const RoiResult& r : rr) for (const TonePeak& p : r.peaks) if (p.snr >= 8) cand.push_back(std::round(p.f * 10) / 10);
        std::sort(cand.begin(), cand.end());
        cand.erase(std::unique(cand.begin(), cand.end()), cand.end());
        for (double fc : cand) {
            bool seen = false;
            for (double c : common) if (std::fabs(fc - c) < 1.0) seen = true;
            if (seen) continue;
            int on = 0, top = 0;
            for (const RoiResult& r : rr) {
                bool has = false;
                for (const TonePeak& p : r.peaks) if (p.snr >= 8 && std::fabs(p.f - fc) < 1.0) has = true;
                on += has;
                if (has && !r.peaks.empty() && std::fabs(r.peaks[0].f - fc) < 1.0) ++top;
            }
            if (on < std::max(3.0, 0.65 * ns)) continue;
            std::vector<double> a;
            for (const RoiResult& r : rr) a.push_back(amp_at(r, fc));
            std::sort(a.rbegin(), a.rend());
            if (top < on / 3.0 && a[0] < 8 * a[2]) common.push_back(fc);
        }
    }
    for (int k = 0; k < ns; ++k) {
        RoiResult& r = rr[k];
        for (const TonePeak& q : r.peaks) {
            bool bad = false;
            for (double c : common) if (std::fabs(q.f - c) < 1.5) bad = true;
            // leakage: another ROI owns this tone (its strongest peak) and is much stronger here
            if (!bad && s.commonMode)
                for (int j = 0; j < ns && !bad; ++j)
                    if (j != k && !rr[j].peaks.empty() && std::fabs(rr[j].peaks[0].f - q.f) < 0.6 && amp_at(rr[j], q.f) >= 2.5 * amp_at(r, q.f)) bad = true;
            if (bad) continue;
            r.tone = q;
            r.ok = q.snr >= s.snrMin;
            break;
        }
    }
    if (commonOut) *commonOut = common;
}

MeasureResult run_measurement(const Clip& c, const std::vector<Roi>& rois, const MeasureSettings& s, const std::vector<Onset>& onsets) {
    MeasureResult res;
    if (!measure_window(c, s, onsets, res.t0, res.t1, &res.note)) return res;
    res.rois.resize(rois.size());
    const unsigned nth = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
    std::vector<std::thread> th;
    for (unsigned w = 0; w < nth; ++w)
        th.emplace_back([&, w] {
            for (size_t k = w; k < rois.size(); k += nth) {
                RoiResult& r = res.rois[k];
                r.id = rois[k].id;
                r.name = rois[k].name;
                r.color = rois[k].color;
                const RoiPixels px = rasterize(rois[k], c.W, c.H);
                r.pixels = (int)px.idx.size();
                r.events = roi_series(c, rois[k], px, s, res.t0, res.t1, r.sig);
                spectrum_of(r.sig, s, r.f, r.amp);
                if (s.rpmOn) {
                    std::vector<double> v;
                    const std::vector<double>* x = &r.sig;
                    if (s.signal == "displacement") { c.roiSignal(px, rois[k].signal, rois[k].sided(), res.t0, res.t1, s.fs, v); x = &v; }
                    r.rpm = estimate_rpm(*x, s.fs, s.rpm());
                }
            }
        });
    for (auto& t : th) t.join();
    pick_tones(res.rois, s, &res.common);
    // decay of the picked tone: ON-OFF rate from the window start, up to 2 s
    th.clear();
    for (unsigned w = 0; w < nth; ++w)
        th.emplace_back([&, w] {
            for (size_t k = w; k < rois.size(); k += nth) {
                RoiResult& r = res.rois[k];
                if (!r.ok) continue;
                const RoiPixels px = rasterize(rois[k], c.W, c.H);
                std::vector<double> v;
                c.roiSignal(px, rois[k].signal, rois[k].sided(), res.t0, std::min(c.duration, res.t0 + 2.0), s.fs, v);
                const hz::dsp::DampFit d = hz::dsp::damping(v, s.fs, r.tone.f);
                if (d.ok && d.zeta > 0) { r.zeta = d.zeta; r.decayS = 1.0 / (2 * M_PI * r.tone.f * d.zeta); }
            }
        });
    for (auto& t : th) t.join();
    return res;
}

Spectrogram2 roi_spectrogram(const Clip& c, const Roi& r, const MeasureSettings& s) {
    Spectrogram2 out;
    const RoiPixels px = rasterize(r, c.W, c.H);
    std::vector<double> v;
    const double tEnd = std::min(c.duration, 60.0);
    c.roiSignal(px, r.signal, r.sided(), 0, tEnd, s.fs, v);
    const int win = std::max(16, (int)std::lround(s.sgWinMs * 1e-3 * s.fs));
    int hop = std::max(1, (int)std::lround(s.sgHopMs * 1e-3 * s.fs));
    if ((int)v.size() / hop > 3000) hop = std::max(1, (int)v.size() / 3000);
    const hz::Spectrogram sg = hz::spectrogram(v.data(), (int)v.size(), s.fs, win, hop, s.fmax);
    out.t = sg.t;
    out.f = sg.f;
    out.nt = sg.n_t;
    out.nf = sg.n_f;
    out.amp.assign(sg.amp.begin(), sg.amp.end());
    for (int i = 0; i < out.nt; ++i) {
        int best = -1;
        double bv = 0;
        for (int k = 0; k < out.nf; ++k) {
            if (out.f[k] < s.toneLo || out.f[k] > s.toneHi) continue;
            const double a = out.amp[(size_t)i * out.nf + k];
            if (a > bv) { bv = a; best = k; }
        }
        if (best >= 0) { out.trackT.push_back(out.t[i]); out.trackF.push_back(out.f[best]); }
    }
    return out;
}

RpmTrack roi_rpm_track(const Clip& c, const Roi& r, const MeasureSettings& s) {
    const RoiPixels px = rasterize(r, c.W, c.H);
    std::vector<double> v;
    c.roiSignal(px, r.signal, r.sided(), 0, std::min(c.duration, 120.0), s.fs, v);
    RpmSettings rs = s.rpm();
    // at most ~600 frames: longer clips get longer frames
    rs.win = std::max(rs.win, std::min(c.duration, 120.0) / 150.0);
    return rpm_track(v, s.fs, 0, rs);
}

// ---- files ------------------------------------------------------------------------------------------------------------------
QString measure_dir(const QString& rawPath) {
    const QString stem = rawPath.endsWith(".raw", Qt::CaseInsensitive) ? rawPath.left(rawPath.size() - 4) : rawPath;
    return stem + ".measure";
}

QString safe_name(const QString& s) {
    QString n = s.trimmed();
    n.replace(QRegularExpression("[^A-Za-z0-9_\\- ]"), "_");
    n = n.simplified();
    n.replace(' ', '_');
    return n.isEmpty() ? QString("measurement") : n.left(80);
}

static QString num(double v, int prec = 6) { return std::isfinite(v) ? QString::number(v, 'g', prec) : QString(); }

bool save_measurement(const QString& rawPath, const QString& name, const std::vector<Roi>& rois, const MeasureSettings& s,
                      const MeasureResult& res, const QImage& map, QString* err) {
    const QString dir = QDir(measure_dir(rawPath)).filePath(safe_name(name));
    if (!QDir().mkpath(dir)) { if (err) *err = "Cannot create " + dir; return false; }
    QJsonObject o;
    QString created = QDateTime::currentDateTime().toString(Qt::ISODate);
    {
        QFile old(QDir(dir).filePath("measurement.json"));
        if (old.open(QIODevice::ReadOnly)) {
            const QJsonObject p = QJsonDocument::fromJson(old.readAll()).object();
            if (p.contains("created")) created = p["created"].toString();
        }
    }
    o["name"] = name.trimmed().isEmpty() ? safe_name(name) : name.trimmed();
    o["clip"] = QFileInfo(rawPath).fileName();
    o["created"] = created;
    o["modified"] = QDateTime::currentDateTime().toString(Qt::ISODate);
    o["app"] = "hertzyevs";
    o["settings"] = s.toJson();
    o["window"] = QJsonObject{{"t0_s", res.t0}, {"t1_s", res.t1}};
    QJsonArray ra;
    for (const Roi& r : rois) ra.append(roi_to_json(r));
    o["rois"] = ra;
    QJsonArray rs;
    for (const RoiResult& r : res.rois) {
        QJsonObject x{{"id", r.id}, {"name", r.name}, {"events", (double)r.events}, {"pixels", r.pixels}, {"ok", r.ok}};
        if (r.tone.f > 0) { x["f_hz"] = r.tone.f; x["snr"] = r.tone.snr; x["amp"] = r.tone.amp; }
        if (std::isfinite(r.zeta)) { x["zeta"] = r.zeta; x["decay_s"] = r.decayS; }
        if (s.rpmOn) {
            x["rpm_ok"] = r.rpm.ok;
            if (r.rpm.ok) { x["rpm"] = r.rpm.rpm; x["rpm_pass_hz"] = r.rpm.pass; x["rpm_snr"] = r.rpm.snr; x["rpm_harmonics"] = r.rpm.found; }
            if (!r.rpm.note.isEmpty()) x["rpm_note"] = r.rpm.note;
        }
        x["floor"] = r.median;
        QJsonArray pk;
        for (const TonePeak& p : r.peaks) pk.append(QJsonArray{std::round(p.f * 100) / 100, std::round(p.snr * 10) / 10});
        x["peaks"] = pk;
        rs.append(x);
    }
    o["results"] = rs;
    QJsonArray cm;
    for (double c : res.common) cm.append(c);
    o["common_mode_hz"] = cm;
    o["units"] = "frequency in Hz; amplitudes relative (event-based, uncalibrated)";
    QFile jf(QDir(dir).filePath("measurement.json"));
    if (!jf.open(QIODevice::WriteOnly)) { if (err) *err = "Cannot write " + jf.fileName(); return false; }
    jf.write(QJsonDocument(o).toJson());
    jf.close();
    // results.csv
    {
        QFile f(QDir(dir).filePath("results.csv"));
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream ts(&f);
            ts << "roi,events,pixels,f_hz,snr,amp_rel,accepted,zeta,decay_s,rpm,rpm_pass_hz,rpm_snr,peaks\n";
            for (const RoiResult& r : res.rois) {
                QStringList pk;
                for (const TonePeak& p : r.peaks) pk << QString("%1/%2").arg(p.f, 0, 'f', 2).arg(p.snr, 0, 'f', 1);
                ts << r.name << "," << r.events << "," << r.pixels << "," << (r.tone.f > 0 ? num(r.tone.f, 7) : "") << ","
                   << (r.tone.f > 0 ? num(r.tone.snr, 4) : "") << "," << (r.tone.f > 0 ? num(r.tone.amp, 5) : "") << "," << (r.ok ? 1 : 0) << ","
                   << num(r.zeta, 4) << "," << num(r.decayS, 4) << ","
                   << (r.rpm.ok ? num(r.rpm.rpm, 7) : "") << "," << (r.rpm.ok ? num(r.rpm.pass, 7) : "") << "," << (r.rpm.ok ? num(r.rpm.snr, 4) : "") << ","
                   << pk.join(' ') << "\n";
            }
        }
    }
    // spectra.csv and signals.csv
    {
        QFile f(QDir(dir).filePath("spectra.csv"));
        size_t n = 0;
        for (const RoiResult& r : res.rois) n = std::max(n, r.f.size());
        if (n && f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream ts(&f);
            ts << "f_hz";
            for (const RoiResult& r : res.rois) ts << "," << r.name;
            ts << "\n";
            const RoiResult* ref = nullptr;
            for (const RoiResult& r : res.rois) if (r.f.size() == n) ref = &r;
            for (size_t k = 0; k < n && ref; ++k) {
                ts << num(ref->f[k], 7);
                for (const RoiResult& r : res.rois) ts << "," << (k < r.amp.size() ? num(r.amp[k], 5) : QString());
                ts << "\n";
            }
        }
    }
    {
        QFile f(QDir(dir).filePath("signals.csv"));
        size_t n = 0;
        for (const RoiResult& r : res.rois) n = std::max(n, r.sig.size());
        if (n && f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream ts(&f);
            ts << "t_s";
            for (const RoiResult& r : res.rois) ts << "," << r.name;
            ts << "\n";
            for (size_t k = 0; k < n; ++k) {
                ts << QString::number(res.t0 + k / s.fs, 'f', 6);
                for (const RoiResult& r : res.rois) ts << "," << (k < r.sig.size() ? num(r.sig[k], 5) : QString());
                ts << "\n";
            }
        }
    }
    if (!map.isNull()) draw_rois(map, rois, &res.rois).save(QDir(dir).filePath("map.png"));
    draw_spectra(res, s, QFileInfo(rawPath).completeBaseName() + "  ·  " + o["name"].toString()).save(QDir(dir).filePath("spectra.png"));
    return true;
}

bool load_measurement(const QString& dir, SavedMeasurement& out) {
    QFile f(QDir(dir).filePath("measurement.json"));
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    if (o.isEmpty()) return false;
    out.name = o["name"].toString(QFileInfo(dir).fileName());
    out.dir = dir;
    out.created = o["modified"].toString(o["created"].toString());
    out.settings = MeasureSettings::fromJson(o["settings"].toObject());
    out.rois = o["rois"].toArray();
    out.results = o["results"].toArray();
    out.t0 = o["window"].toObject()["t0_s"].toDouble();
    out.t1 = o["window"].toObject()["t1_s"].toDouble();
    out.mapKind = o["map"].toString();
    return true;
}

std::vector<SavedMeasurement> list_measurements(const QString& rawPath) {
    std::vector<SavedMeasurement> v;
    const QDir d(measure_dir(rawPath));
    if (!d.exists()) return v;
    for (const QFileInfo& fi : d.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Time)) {
        SavedMeasurement m;
        if (load_measurement(fi.absoluteFilePath(), m)) v.push_back(std::move(m));
    }
    return v;
}

// ---- figures -------------------------------------------------------------------------------------------------------------------
QImage draw_rois(const QImage& map, const std::vector<Roi>& rois, const std::vector<RoiResult>* res) {
    QImage img = map.convertToFormat(QImage::Format_ARGB32);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    QFont font("Sans");
    font.setPixelSize(13);
    font.setBold(true);
    p.setFont(font);
    for (size_t k = 0; k < rois.size(); ++k) {
        const Roi& r = rois[k];
        QColor c = r.color.isValid() ? r.color : roi_color((int)k);
        p.setPen(QPen(c, 1.4));
        p.setBrush(Qt::NoBrush);
        QPointF lab;
        if (r.type == RoiType::Line && r.pts.size() >= 2) {
            const QPointF a = r.pts[0], b = r.pts[1];
            const double L = std::max(1e-9, r.length());
            const QPointF n(-(b.y() - a.y()) / L * r.half, (b.x() - a.x()) / L * r.half);
            p.drawPolygon(QPolygonF({a + n, b + n, b - n, a - n}));
            p.setPen(QPen(c, 1, Qt::DashLine));
            p.drawLine(a, b);
            lab = a + QPointF(6, -6);
        } else if (r.type == RoiType::Rect && r.pts.size() >= 2) {
            p.drawRect(QRectF(r.pts[0], r.pts[1]).normalized());
            lab = QRectF(r.pts[0], r.pts[1]).normalized().topLeft() + QPointF(3, -5);
        } else if (r.type == RoiType::Polygon && r.pts.size() >= 3) {
            p.drawPolygon(QPolygonF(QList<QPointF>(r.pts.begin(), r.pts.end())));
            lab = r.pts[0] + QPointF(4, -5);
        } else if (r.type == RoiType::Mask && r.maskW == img.width() && r.maskH == img.height()) {
            QColor t = c;
            const int W = img.width();
            for (int y = 0; y < img.height(); ++y) {
                QRgb* row = reinterpret_cast<QRgb*>(img.scanLine(y));
                for (int x = 0; x < W; ++x)
                    if (r.mask[(size_t)y * W + x]) row[x] = qRgb((qRed(row[x]) + t.red()) / 2, (qGreen(row[x]) + t.green()) / 2, (qBlue(row[x]) + t.blue()) / 2);
            }
            lab = r.bounds().topLeft() + QPointF(2, -4);
        }
        QString text = r.name;
        if (res) for (const RoiResult& rr : *res) if (rr.id == r.id && rr.ok) text += QString("  %1 Hz").arg(rr.tone.f, 0, 'f', 1);
        p.setPen(QPen(QColor(0, 0, 0, 200), 3));
        QPainterPath path;
        path.addText(lab, font, text);
        p.strokePath(path, QPen(QColor(0, 0, 0, 220), 3));
        p.fillPath(path, c);
    }
    return img;
}

QImage draw_spectra(const MeasureResult& res, const MeasureSettings& s, const QString& title, int w) {
    const int n = std::max<int>(1, (int)res.rois.size()), rowH = 120, top = 44, left = 70, right = 20;
    QImage img(w, top + n * rowH + 40, QImage::Format_RGB32);
    img.fill(Qt::white);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    QFont f("Sans");
    f.setPixelSize(13);
    p.setFont(f);
    p.setPen(Qt::black);
    p.drawText(QRectF(10, 6, w - 20, 34), Qt::AlignLeft | Qt::AlignVCenter,
               title + QString("   ·   window %1–%2 s, %3 signal, %4 window, linear amplitude (relative)")
                           .arg(res.t0, 0, 'f', 3).arg(res.t1, 0, 'f', 3).arg(s.signal).arg(s.window));
    const double f0 = std::max(0.0, s.bandLo * 0.5), f1 = std::min(s.fmax, std::max(s.bandHi, s.toneHi));
    const int pw = w - left - right;
    for (int k = 0; k < (int)res.rois.size(); ++k) {
        const RoiResult& r = res.rois[k];
        const int y0 = top + k * rowH, ph = rowH - 26;
        p.setPen(QColor(200, 200, 200));
        p.drawRect(left, y0, pw, ph);
        p.fillRect(QRectF(left + (s.toneLo - f0) / (f1 - f0) * pw, y0 + 1, (s.toneHi - s.toneLo) / (f1 - f0) * pw, ph - 1), QColor(240, 246, 255));
        double mx = 1e-12;
        for (size_t i = 0; i < r.f.size(); ++i) if (r.f[i] >= f0 && r.f[i] <= f1) mx = std::max(mx, r.amp[i]);
        QPainterPath path;
        bool first = true;
        for (size_t i = 0; i < r.f.size(); ++i) {
            if (r.f[i] < f0 || r.f[i] > f1) continue;
            const QPointF q(left + (r.f[i] - f0) / (f1 - f0) * pw, y0 + ph - r.amp[i] / mx * (ph - 6));
            if (first) path.moveTo(q); else path.lineTo(q);
            first = false;
        }
        p.setPen(QPen(r.ok ? QColor(30, 100, 200) : QColor(140, 140, 140), 1.0));
        p.drawPath(path);
        for (double c : res.common) {
            const double x = left + (c - f0) / (f1 - f0) * pw;
            p.setPen(QPen(QColor(150, 150, 150), 1, Qt::DotLine));
            p.drawLine(QPointF(x, y0), QPointF(x, y0 + ph));
        }
        if (r.tone.f > 0) {
            const double x = left + (r.tone.f - f0) / (f1 - f0) * pw;
            p.setPen(QPen(r.ok ? QColor(220, 40, 40) : QColor(200, 150, 150), 1));
            p.drawLine(QPointF(x, y0), QPointF(x, y0 + ph));
        }
        p.setPen(Qt::black);
        p.drawText(QRectF(4, y0, left - 8, ph), Qt::AlignRight | Qt::AlignVCenter, r.name);
        QString t = r.tone.f > 0 ? QString("%1 Hz  SNR %2%3").arg(r.tone.f, 0, 'f', 2).arg(r.tone.snr, 0, 'f', 0).arg(r.ok ? "" : "  (below the SNR limit)")
                                 : QString("no tone");
        if (std::isfinite(r.decayS)) t += QString("   decay %1 ms").arg(r.decayS * 1000, 0, 'f', 0);
        p.drawText(QRectF(left, y0 + 2, pw - 6, 18), Qt::AlignRight | Qt::AlignTop, t);
        // axis
        p.setPen(QColor(90, 90, 90));
        for (double fx = std::ceil(f0 / 100) * 100; fx <= f1; fx += 100) {
            const double x = left + (fx - f0) / (f1 - f0) * pw;
            p.drawLine(QPointF(x, y0 + ph), QPointF(x, y0 + ph + 4));
            if (k == (int)res.rois.size() - 1 || (int(fx) % 200 == 0)) p.drawText(QRectF(x - 30, y0 + ph + 4, 60, 16), Qt::AlignHCenter, QString::number(fx, 'f', 0));
        }
    }
    p.drawText(QRectF(left, img.height() - 22, pw, 18), Qt::AlignHCenter, "frequency (Hz)");
    return img;
}

}  // namespace evs

#include "rpm.hpp"
#include <algorithm>
#include <complex>
#include <numeric>
#include "pocketfft_hdronly.h"

namespace evs {

namespace {

// noise floor per frequency: median of 1/4-octave segments, each at least 12 resolution widths wide (a zero-padded line
// is ~4 resolution widths wide, so lines never fill half a segment), so a sloped event spectrum is handled
struct Floor {
    std::vector<double> edge, med;   // segment start frequencies and medians
    double at(double f) const {
        if (med.empty()) return 1e-30;
        const size_t i = std::upper_bound(edge.begin(), edge.end(), f) - edge.begin();
        return med[i == 0 ? 0 : std::min(i - 1, med.size() - 1)];
    }
};

Floor make_floor(const std::vector<double>& f, const std::vector<double>& a, size_t from, double res) {
    Floor fl;
    if (f.size() < 4) return fl;
    const double df = f[1] - f[0];
    size_t i = std::max<size_t>(from, 1);
    std::vector<double> tmp;
    while (i < f.size()) {
        size_t j = std::max(i + std::max<size_t>(24, (size_t)std::ceil(12 * res / df)), (size_t)std::ceil(f[i] * std::pow(2.0, 0.25) / df));
        j = std::min(j, f.size());
        if (f.size() - j < 12) j = f.size();
        tmp.assign(a.begin() + i, a.begin() + j);
        std::nth_element(tmp.begin(), tmp.begin() + tmp.size() / 2, tmp.end());
        fl.edge.push_back(i == std::max<size_t>(from, 1) ? 0.0 : f[i]);
        fl.med.push_back(std::max(tmp[tmp.size() / 2], 1e-30));
        i = j;
    }
    return fl;
}

// strongest bin within [lo, hi] (Hz); -1 if outside the spectrum
int peak_in(const std::vector<double>& f, const std::vector<double>& a, double lo, double hi) {
    if (f.size() < 3) return -1;
    const double df = f[1] - f[0];
    const int i0 = std::max(1, (int)std::floor(lo / df)), i1 = std::min((int)f.size() - 2, (int)std::ceil(hi / df));
    if (i1 < i0) return -1;
    int b = i0;
    for (int i = i0 + 1; i <= i1; ++i) if (a[i] > a[b]) b = i;
    return b;
}

// parabolic interpolation of a peak bin
double refine(const std::vector<double>& f, const std::vector<double>& a, int i) {
    if (i <= 0 || i >= (int)a.size() - 1) return f[std::clamp(i, 0, (int)f.size() - 1)];
    const double y0 = a[i - 1], y1 = a[i], y2 = a[i + 1], den = y0 - 2 * y1 + y2;
    const double d = std::fabs(den) > 1e-30 ? std::clamp(0.5 * (y0 - y2) / den, -0.5, 0.5) : 0.0;
    return f[i] + d * (f[1] - f[0]);
}

}  // namespace

RpmResult rpm_from_spectrum(const std::vector<double>& f, const std::vector<double>& amp, const RpmSettings& s, double res) {
    RpmResult r;
    if (f.size() < 16) { r.note = "no signal"; return r; }
    const double df = f[1] - f[0], fTop = f.back();
    const int marks = std::max(1, s.marks), K = std::clamp(s.harmonics, 1, 20);
    // the lowest repetition can be the shaft (rpmLo / 60) or the pass rate; lines are looked for up to K pass harmonics
    const double fA = std::max(3 * df, s.rpmLo / 60.0 * 0.98), fB = std::min(fTop / 1.01, marks * s.rpmHi / 60.0 * K);
    if (fB <= fA) { r.note = "speed range outside the spectrum (raise the sampling rate or lower the range)"; return r; }
    const Floor fl = make_floor(f, amp, (size_t)std::max(1.0, std::floor(fA * 0.5 / df)), res > 0 ? res : 8 * df);
    const double tol = 0.002;   // relative speed wander allowed between harmonics
    auto snrAt = [&](double x, int* bin = nullptr) {
        const int b = peak_in(f, amp, x * (1 - tol) - df, x * (1 + tol) + df);
        if (bin) *bin = b;
        return b < 0 ? 0.0 : amp[b] / fl.at(f[b]);
    };
    const QString range = QString("%1–%2 rpm").arg(s.rpmLo, 0, 'f', 0).arg(s.rpmHi, 0, 'f', 0);

    // 1. the strongest line: a harmonic of the repetition frequency f0
    int bp = -1;
    double best = 0;
    for (int i = std::max(1, (int)std::ceil(fA / df)); i <= (int)std::floor(fB / df) && i + 1 < (int)amp.size(); ++i) {
        if (amp[i] < amp[i - 1] || amp[i] < amp[i + 1]) continue;
        const double sn = amp[i] / fl.at(f[i]);
        if (sn > best) { best = sn; bp = i; }
    }
    if (bp < 0 || best < s.snrMin) { r.note = QString("no periodic line above SNR %1 for %2").arg(s.snrMin, 0, 'f', 0).arg(range); return r; }
    const double fp = refine(f, amp, bp);

    // 2. the lowest f0 = fp / m that is a real line itself (SNR >= the limit) with a share of its first harmonics below fp
    double f0 = fp;
    const int mMax = std::min(64, (int)std::floor(fp / fA));
    for (int m = mMax; m >= 2; --m) {
        const double c = fp / m;
        if (snrAt(c) < s.snrMin) continue;
        const int J = std::min(m - 1, 12);
        int hit = 0;
        for (int j = 1; j <= J; ++j) hit += snrAt(j * c) >= 3;
        if (hit >= std::ceil(0.34 * J)) { f0 = c; break; }   // a third: pulse pairs can have odd harmonics only
    }

    // 3. refine f0 from its harmonics (weighted mean of f_k / k; each estimate narrows the window for the next)
    int b1 = -1;
    snrAt(f0, &b1);
    if (b1 > 0) f0 = refine(f, amp, b1);
    const double snr0 = b1 > 0 ? amp[b1] / fl.at(f[b1]) : 0;
    double wsum = snr0, psum = snr0 * f0, bestK = snr0;
    std::vector<std::pair<double, double>> est{{f0, snr0}};
    std::vector<double> hs{snr0};   // SNR of harmonic k (index k - 1)
    int strongest = 1, found = 1;
    const int Kh = std::min(40, (int)std::floor(fB / f0));
    for (int k = 2; k <= Kh; ++k) {
        const double x = k * f0, w = std::max(2 * df, x * tol);
        const int b = peak_in(f, amp, x - w, x + w);
        const double sn = b < 0 ? 0.0 : amp[b] / fl.at(f[b]);
        hs.push_back(sn);
        if (sn < 3) continue;
        const double pk = refine(f, amp, b) / k, wk = sn * k;
        est.push_back({pk, wk});
        wsum += wk;
        psum += wk * pk;
        f0 = psum / wsum;
        ++found;
        if (sn > bestK) { bestK = sn; strongest = k; }
    }
    double var = 0;
    for (const auto& e : est) var += e.second * (e.first - f0) * (e.first - f0);
    r.spread = est.size() > 1 ? std::sqrt(var / wsum) / f0 : (df / 4) / f0;
    r.found = found;
    r.strongest = strongest;
    r.snr = snr0;

    // 4. what is f0? With several marks it is the pass rate, unless the lines on every marks-th harmonic stand out: then
    //    f0 is the shaft (the marks are not all alike, e.g. imbalance or one odd blade) and the passes are marks x f0
    bool shaft = marks == 1;
    if (marks > 1) {
        double sp = 0, so = 0;
        int np = 0, no = 0;
        for (int k = 1; k <= (int)hs.size(); ++k) {
            if (k % marks == 0) { sp += hs[k - 1]; ++np; } else { so += hs[k - 1]; ++no; }
        }
        if (np > 0 && no > 0 && sp / np >= 1.5 * (so / no) && hs.size() >= (size_t)marks && hs[marks - 1] >= 2 * snr0) shaft = true;
    }
    r.pass = shaft ? f0 * marks : f0;
    r.rpm = 60.0 * r.pass / marks;
    r.shaftSnr = shaft && marks > 1 ? snr0 : 0;
    QStringList notes;
    if (marks > 1 && shaft) notes << QString("once-per-turn line at %1 Hz (SNR %2), passes at %3 Hz").arg(f0, 0, 'f', 2).arg(snr0, 0, 'f', 0).arg(r.pass, 0, 'f', 2);
    if (marks == 1 && strongest >= 3 && strongest <= 24 && bestK >= 3 * snr0)
        notes << QString("strongest line is harmonic %1 (%2 Hz): %1 blades / teeth plus one mark?").arg(strongest).arg(strongest * f0, 0, 'f', 1);
    if (r.rpm < s.rpmLo * 0.98 || r.rpm > s.rpmHi * 1.02) {
        r.note = QString("periodic at %1 Hz = %2 rpm, outside %3").arg(f0, 0, 'f', 2).arg(r.rpm, 0, 'f', 1).arg(range);
        return r;
    }
    r.ok = true;
    r.note = notes.join("; ");
    return r;
}

RpmResult estimate_rpm(const std::vector<double>& x, double fs, const RpmSettings& s) {
    const int n = (int)x.size();
    if (n < 32) { RpmResult r; r.note = "window too short"; return r; }
    const int marks = std::max(1, s.marks);
    const double need = std::min(fs / 2, std::max(1.0, marks * s.rpmHi / 60.0) * (std::clamp(s.harmonics, 1, 20) + 0.5));
    size_t N = 1;
    while (N < (size_t)n * 8) N <<= 1;
    std::vector<double> buf(N, 0.0);
    const double mean = std::accumulate(x.begin(), x.end(), 0.0) / n;
    double wsum = 0;
    for (int i = 0; i < n; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2 * M_PI * i / (n - 1));
        buf[i] = (x[i] - mean) * w;
        wsum += w;
    }
    std::vector<std::complex<double>> X(N / 2 + 1);
    pocketfft::r2c<double>({N}, {sizeof(double)}, {sizeof(std::complex<double>)}, 0, pocketfft::FORWARD, buf.data(), X.data(), 1.0);
    std::vector<double> f, a;
    const double df = fs / N;
    for (size_t k = 0; k < X.size() && k * df <= need; ++k) { f.push_back(k * df); a.push_back(std::abs(X[k]) * 2 / wsum); }
    RpmResult r = rpm_from_spectrum(f, a, s, fs / n);
    // a period must fit the window a few times, else the line is too wide to trust
    if (r.ok && r.pass * n / fs < 3) { r.ok = false; r.note = QString("window holds only %1 passes: use a longer window").arg(r.pass * n / fs, 0, 'f', 1); }
    return r;
}

RpmTrack rpm_track(const std::vector<double>& x, double fs, double t0, const RpmSettings& s) {
    RpmTrack tr;
    const int win = std::max(64, (int)std::lround(s.win * fs)), hop = std::max(16, win / 4);
    if ((int)x.size() < win) return tr;
    std::vector<double> seg;
    for (int a = 0; a + win <= (int)x.size(); a += hop) {
        seg.assign(x.begin() + a, x.begin() + a + win);
        const RpmResult r = estimate_rpm(seg, fs, s);
        tr.t.push_back(t0 + (a + win / 2.0) / fs);
        tr.rpm.push_back(r.ok ? r.rpm : NAN);
        tr.snr.push_back(r.snr);
    }
    return tr;
}

}  // namespace evs

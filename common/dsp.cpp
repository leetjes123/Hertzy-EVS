#include "dsp.hpp"
#include <algorithm>
#include <cmath>
#include <complex>
#include <numeric>
#include "pocketfft_hdronly.h"

namespace hz::dsp {
using cd = std::complex<double>;

static double response(const SosChain& s, double w) {
    const cd z1 = std::polar(1.0, -w), z2 = z1 * z1;
    cd h = 1;
    for (const Sos& q : s) h *= (q[0] + q[1] * z1 + q[2] * z2) / (1.0 + q[4] * z1 + q[5] * z2);
    return std::abs(h);
}

SosChain butter(int order, double hp, double lp, double fs) {
    const double nyq = 0.49 * fs;
    hp = (hp > 0 && hp < nyq) ? hp : 0;
    lp = (lp > 0 && lp < nyq) ? lp : 0;
    if (hp && lp && hp >= lp) lp = 0;
    if (!hp && !lp) return {};
    const double k = 2 * fs;
    const auto warp = [&](double f) { return k * std::tan(M_PI * f / fs); };
    std::vector<cd> poles, proto;
    for (int i = 0; i < order; ++i) proto.push_back(std::polar(1.0, M_PI * (2.0 * i + order + 1) / (2.0 * order)));
    int zeros_p1 = 0, zeros_m1 = 0;       // zeros at z = +1 and z = -1
    double ref_w = 0;                     // frequency with unit gain
    if (hp && lp) {
        const double w1 = warp(hp), w2 = warp(lp), w0 = std::sqrt(w1 * w2), bw = w2 - w1;
        for (const cd& p : proto) {
            const cd a = p * bw / 2.0, r = std::sqrt(a * a - w0 * w0);
            poles.push_back(a + r);
            poles.push_back(a - r);
        }
        zeros_p1 = zeros_m1 = order;
        ref_w = 2 * std::atan(w0 / k);
    } else if (hp) {
        for (const cd& p : proto) poles.push_back(warp(hp) / p);
        zeros_p1 = order;
        ref_w = M_PI;
    } else {
        for (const cd& p : proto) poles.push_back(p * warp(lp));
        zeros_m1 = order;
    }
    for (cd& p : poles) p = (k + p) / (k - p);

    // conjugate pole pairs, one biquad each (upper half plane only: the other half are the conjugates)
    SosChain out;
    for (const cd& p : poles) {
        if (p.imag() <= 0) continue;
        Sos q{1, 0, 0, 1, -2 * p.real(), std::norm(p)};
        double a = 0, b = 0;              // the two zeros of this section
        if (zeros_p1 > 0 && zeros_m1 > 0) { a = 1; b = -1; --zeros_p1; --zeros_m1; }
        else if (zeros_p1 > 1) { a = b = 1; zeros_p1 -= 2; }
        else { a = b = -1; zeros_m1 -= 2; }
        q[1] = -(a + b);
        q[2] = a * b;
        out.push_back(q);
    }
    const double g = 1.0 / response(out, ref_w);
    for (int i = 0; i < 3; ++i) out[0][i] *= g;
    return out;
}

Sos iir_notch(double f, double bw, double fs) {
    const double w0 = 2 * M_PI * f / fs, beta = std::tan(M_PI * bw / fs), g = 1 / (1 + beta);
    return {g, -2 * g * std::cos(w0), g, 1, -2 * g * std::cos(w0), 2 * g - 1};
}

void add_mains_notches(SosChain& chain, double mains, double hp, double upto, double fs, double bw) {
    const double nyq = 0.49 * fs;
    for (double f = mains; f < upto * 1.1; f += mains)
        if (f >= 0.8 * hp && f < nyq) chain.push_back(iir_notch(f, bw, fs));
}

void sosfilt(const SosChain& s, const float* x, float* y, int n, SosState& z) {
    if (z.size() != s.size()) z.assign(s.size(), {0.0, 0.0});
    for (int i = 0; i < n; ++i) {
        double v = x[i];
        for (size_t k = 0; k < s.size(); ++k) {
            const Sos& q = s[k];
            const double o = q[0] * v + z[k][0];
            z[k][0] = q[1] * v - q[4] * o + z[k][1];
            z[k][1] = q[2] * v - q[5] * o;
            v = o;
        }
        y[i] = (float)v;
    }
}

static void run(const SosChain& s, std::vector<double>& x, SosState z) {
    for (double& sample : x) {
        double v = sample;
        for (size_t k = 0; k < s.size(); ++k) {
            const Sos& q = s[k];
            const double o = q[0] * v + z[k][0];
            z[k][0] = q[1] * v - q[4] * o + z[k][1];
            z[k][1] = q[2] * v - q[5] * o;
            v = o;
        }
        sample = v;
    }
}

// steady-state state for a unit step at the input of the chain (scipy.signal.sosfilt_zi)
static SosState step_state(const SosChain& s, double level) {
    SosState z(s.size());
    double scale = level;
    for (size_t k = 0; k < s.size(); ++k) {
        const Sos& q = s[k];
        const double y = (q[0] + q[1] + q[2]) / (1 + q[4] + q[5]);
        z[k][1] = scale * (q[2] - q[5] * y);
        z[k][0] = scale * (q[1] - q[4] * y) + z[k][1];
        scale *= y;
    }
    return z;
}

std::vector<double> sosfiltfilt(const SosChain& s, const std::vector<double>& x) {
    if (s.empty()) return x;
    const int n = (int)x.size();
    int edge = (2 * (int)s.size() + 1) * 3;
    if (n <= edge) return x;
    std::vector<double> e(n + 2 * edge);
    for (int i = 0; i < edge; ++i) {
        e[i] = 2 * x[0] - x[edge - i];
        e[n + edge + i] = 2 * x[n - 1] - x[n - 2 - i];
    }
    std::copy(x.begin(), x.end(), e.begin() + edge);
    const double x0 = e[0];
    run(s, e, step_state(s, x0));
    std::reverse(e.begin(), e.end());
    const double y0 = e[0];
    run(s, e, step_state(s, y0));
    std::reverse(e.begin(), e.end());
    return std::vector<double>(e.begin() + edge, e.begin() + edge + n);
}

static std::vector<double> envelope(const std::vector<double>& y) {
    const size_t n = y.size();
    std::vector<cd> in(n), X(n), a(n);
    for (size_t i = 0; i < n; ++i) in[i] = y[i];
    pocketfft::shape_t shape{n};
    pocketfft::stride_t st{sizeof(cd)};
    pocketfft::c2c(shape, st, st, {0}, pocketfft::FORWARD, in.data(), X.data(), 1.0);
    for (size_t i = 1; i < n; ++i) X[i] *= (2 * i < n) ? 2.0 : (2 * i == n) ? 1.0 : 0.0;
    pocketfft::c2c(shape, st, st, {0}, pocketfft::BACKWARD, X.data(), a.data(), 1.0 / n);
    std::vector<double> m(n);
    for (size_t i = 0; i < n; ++i) m[i] = std::abs(a[i]);
    return m;
}

DampFit damping(const std::vector<double>& x, double fs, double f0, std::vector<float>* env_out) {
    DampFit r;
    const double lo = f0 * 0.9, hi = std::min(f0 * 1.1, 0.45 * fs);
    if (f0 < 10 || hi <= lo || x.size() < 64) return r;
    const double mean = std::accumulate(x.begin(), x.end(), 0.0) / x.size();
    std::vector<double> c(x);
    for (double& v : c) v -= mean;
    const std::vector<double> y = sosfiltfilt(butter(4, lo, hi, fs), c);
    const std::vector<double> e0 = envelope(y);
    const int n = (int)e0.size(), k = std::max(1, (int)(2 * fs / f0));

    // moving average, numpy.convolve(..., "same") alignment
    std::vector<double> cum(n + 1, 0.0), env(n);
    for (int i = 0; i < n; ++i) cum[i + 1] = cum[i] + e0[i];
    for (int i = 0; i < n; ++i) {
        const int hi_i = std::min(n, i + (k - 1) / 2 + 1), lo_i = std::max(0, hi_i - k);
        env[i] = (cum[hi_i] - cum[lo_i]) / k;
    }
    if (env_out) env_out->assign(env.begin(), env.end());
    std::vector<double> db(n);
    for (int i = 0; i < n; ++i) db[i] = 20 * std::log10(env[i] + 1e-12);
    const int end = n - k;
    if (end < 4) return r;
    const int top = (int)(std::max_element(db.begin(), db.begin() + end) - db.begin());
    int i0 = -1, i1 = -1;
    for (int i = top; i < end; ++i) {
        if (i0 < 0 && db[i] < db[top] - 3) i0 = i;
        if (i1 < 0 && db[i] < db[top] - 25) { i1 = i; break; }
    }
    if (i0 < 0) i0 = top;
    if (i1 < 0) i1 = end - 1;
    if (i1 - i0 < 0.02 * fs) return r;

    double st = 0, sy = 0, stt = 0, sty = 0;
    const int m = i1 - i0;
    for (int i = i0; i < i1; ++i) {
        const double t = i / fs;
        st += t; sy += db[i]; stt += t * t; sty += t * db[i];
    }
    r.slope = (m * sty - st * sy) / (m * stt - st * st);
    r.icpt = (sy - r.slope * st) / m;
    r.zeta = -r.slope / (20 / std::log(10.0)) / (2 * M_PI * f0);
    r.ok = true;
    r.i0 = i0; r.i1 = i1;
    return r;
}

DbPeak interp_peak(const std::vector<float>& db, double df, int i) {
    if (i <= 0 || i >= (int)db.size() - 1) return {i * df, (double)db[i]};
    const double a = db[i - 1], b = db[i], c = db[i + 1], den = a - 2 * b + c;
    const double p = den != 0 ? 0.5 * (a - c) / den : 0.0;
    return {(i + p) * df, b - 0.25 * (a - c) * p};
}

std::vector<DbPeak> find_peaks_db(const std::vector<float>& db, double df, double fmin, double fmax, int distance,
                                  int n, double prom, double floor) {
    const int i0 = (int)(fmin / df), i1 = std::min((int)(fmax / df) + 1, (int)db.size());
    std::vector<DbPeak> out;
    if (i1 - i0 < 3 || i0 < 0) return out;
    const float top = *std::max_element(db.begin() + i0, db.begin() + i1);
    std::vector<int> idx;
    for (int i = i0 + 1; i < i1 - 1; ++i) {           // strict local maxima, plateau midpoints
        if (!(db[i] > db[i - 1])) continue;
        int j = i;
        while (j + 1 < i1 - 1 && db[j + 1] == db[i]) ++j;
        if (db[j + 1] < db[i]) { const int mid = (i + j) / 2; if (db[mid] >= top - floor) idx.push_back(mid); }
        i = j;
    }
    // distance: strongest first, neighbours closer than `distance` bins are dropped
    std::vector<int> order(idx.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](int a, int b) { return db[idx[a]] > db[idx[b]]; });
    std::vector<char> keep(idx.size(), 1);
    for (int o : order) {
        if (!keep[o]) continue;
        for (int j = o - 1; j >= 0 && idx[o] - idx[j] < distance; --j) keep[j] = 0;
        for (int j = o + 1; j < (int)idx.size() && idx[j] - idx[o] < distance; ++j) keep[j] = 0;
    }
    std::vector<int> pk;
    for (int o : order) {
        if (!keep[o]) continue;
        const int i = idx[o];
        float lmin = db[i], rmin = db[i];
        for (int j = i - 1; j >= i0 && db[j] <= db[i]; --j) lmin = std::min(lmin, db[j]);
        for (int j = i + 1; j < i1 && db[j] <= db[i]; ++j) rmin = std::min(rmin, db[j]);
        if (db[i] - std::max(lmin, rmin) >= prom) pk.push_back(i);
        if ((int)pk.size() == n) break;
    }
    for (int i : pk) out.push_back(interp_peak(db, df, i));
    return out;
}

bool peak_near(const std::vector<float>& db, double df, double f, DbPeak& out, double rel) {
    const int i0 = std::max(0, (int)(f * (1 - rel) / df)), i1 = std::min((int)(f * (1 + rel) / df) + 1, (int)db.size());
    if (i1 - i0 < 3) return false;
    out = interp_peak(db, df, (int)(std::max_element(db.begin() + i0, db.begin() + i1) - db.begin()));
    return true;
}

}  // namespace hz::dsp

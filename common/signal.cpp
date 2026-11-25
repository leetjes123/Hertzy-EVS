#include "signal.hpp"
#include <algorithm>
#include <cmath>
#include "pocketfft_hdronly.h"

namespace hz {
using cd = std::complex<double>;

static void rfft(const std::vector<double>& in, std::vector<cd>& out) {
    out.resize(in.size() / 2 + 1);
    pocketfft::shape_t shape{in.size()};
    pocketfft::stride_t si{sizeof(double)}, so{sizeof(cd)};
    pocketfft::r2c(shape, si, so, 0, pocketfft::FORWARD, in.data(), out.data(), 1.0);
}
static void irfft(const std::vector<cd>& in, std::vector<double>& out, size_t n) {
    out.resize(n);
    pocketfft::shape_t shape{n};
    pocketfft::stride_t si{sizeof(cd)}, so{sizeof(double)};
    pocketfft::c2r(shape, si, so, 0, pocketfft::BACKWARD, in.data(), out.data(), 1.0 / n);
}

Spectrum amplitude_spectrum(const double* x, int n, double fs, int pad, bool hann) {
    Spectrum s;
    if (n < 4) return s;
    size_t N = 1;
    while (N < (size_t)n * std::max(pad, 1)) N <<= 1;
    std::vector<double> buf(N, 0.0);
    double mean = 0, wsum = 0;
    for (int i = 0; i < n; ++i) mean += x[i];
    mean /= n;
    for (int i = 0; i < n; ++i) {
        double wv = hann ? 0.5 - 0.5 * std::cos(2 * M_PI * i / (n - 1)) : 1.0;
        buf[i] = (x[i] - mean) * wv;
        wsum += wv;
    }
    std::vector<cd> X;
    rfft(buf, X);
    s.f.resize(X.size()); s.amp.resize(X.size());
    for (size_t k = 0; k < X.size(); ++k) { s.f[k] = k * fs / N; s.amp[k] = 2 * std::abs(X[k]) / wsum; }
    return s;
}

Spectrogram spectrogram(const double* x, int n, double fs, int win, int hop, double fmax) {
    Spectrogram sg;
    if (n < win || win < 8) return sg;
    size_t N = 1;
    while (N < (size_t)win * 4) N <<= 1;
    std::vector<double> wv(win), buf(N);
    double wsum = 0;
    for (int i = 0; i < win; ++i) { wv[i] = 0.5 - 0.5 * std::cos(2 * M_PI * i / (win - 1)); wsum += wv[i]; }
    int nbins = std::min<int>(N / 2 + 1, (int)(fmax * N / fs) + 1);
    for (int k = 0; k < nbins; ++k) sg.f.push_back(k * fs / N);
    std::vector<cd> X;
    for (int s0 = 0; s0 + win <= n; s0 += hop) {
        std::fill(buf.begin(), buf.end(), 0.0);
        double mean = 0;
        for (int i = 0; i < win; ++i) mean += x[s0 + i];
        mean /= win;
        for (int i = 0; i < win; ++i) buf[i] = (x[s0 + i] - mean) * wv[i];
        rfft(buf, X);
        for (int k = 0; k < nbins; ++k) sg.amp.push_back(2 * std::abs(X[k]) / wsum);
        sg.t.push_back((s0 + win / 2.0) / fs);
    }
    sg.n_t = (int)sg.t.size();
    sg.n_f = nbins;
    return sg;
}

std::vector<double> bandpass(const double* x, int n, double fs, double f_lo, double f_hi, double taper) {
    size_t N = 1;
    while (N < (size_t)n * 2) N <<= 1;     // zero padding against wrap-around
    std::vector<double> buf(N, 0.0), out;
    std::copy(x, x + n, buf.begin());
    std::vector<cd> X;
    rfft(buf, X);
    auto edge = [&](double f, double f0, bool rising) {
        if (taper <= 0) return (rising ? f >= f0 : f <= f0) ? 1.0 : 0.0;
        double t = (f - (f0 - taper / 2)) / taper;
        t = std::clamp(t, 0.0, 1.0);
        double r = 0.5 - 0.5 * std::cos(M_PI * t);
        return rising ? r : 1 - r;
    };
    for (size_t k = 0; k < X.size(); ++k) {
        double f = k * fs / N, g = 1.0;
        if (f_lo > 0) g *= edge(f, f_lo, true);
        if (f_hi > 0) g *= edge(f, f_hi, false);
        X[k] *= g;
    }
    irfft(X, out, N);
    out.resize(n);
    return out;
}

cd dft_at(const double* x, int n, double fs, double f) {
    cd acc = 0;
    double wsum = 0, w = 2 * M_PI * f / fs;
    cd rot = std::polar(1.0, -w), ph = 1.0;
    for (int i = 0; i < n; ++i) {
        double wv = 0.5 - 0.5 * std::cos(2 * M_PI * i / (n - 1));
        acc += x[i] * wv * ph;
        ph *= rot;
        wsum += wv;
    }
    return 2.0 * acc / wsum;
}

std::vector<Peak> find_peaks(const Spectrum& s, double fmin, double fmax, int nmax, double rel) {
    std::vector<Peak> p;
    double top = 0;
    for (size_t k = 0; k < s.f.size(); ++k) if (s.f[k] >= fmin && s.f[k] <= fmax) top = std::max(top, s.amp[k]);
    for (size_t k = 1; k + 1 < s.f.size(); ++k) {
        if (s.f[k] < fmin || s.f[k] > fmax) continue;
        if (s.amp[k] > s.amp[k - 1] && s.amp[k] >= s.amp[k + 1] && s.amp[k] >= rel * top) {
            // parabolic interpolation of the peak
            double y0 = s.amp[k - 1], y1 = s.amp[k], y2 = s.amp[k + 1], den = y0 - 2 * y1 + y2;
            double d = den != 0 ? 0.5 * (y0 - y2) / den : 0;
            p.push_back({s.f[k] + d * (s.f[1] - s.f[0]), y1 - 0.25 * (y0 - y2) * d});
        }
    }
    std::sort(p.begin(), p.end(), [](const Peak& a, const Peak& b) { return a.amp > b.amp; });
    if ((int)p.size() > nmax) p.resize(nmax);
    return p;
}

}  // namespace hz

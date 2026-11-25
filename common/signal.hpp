// Signal tools: windowed amplitude spectrum, spectrogram, zero-phase FFT band-pass, single-frequency DFT, peaks.
#pragma once
#include <complex>
#include <vector>

namespace hz {

struct Spectrum { std::vector<double> f, amp; };
// Hann window, zero-padded to the next power of two >= n*pad; amplitude-calibrated (a sine of amplitude A -> A)
Spectrum amplitude_spectrum(const double* x, int n, double fs, int pad = 8, bool hann = true);

struct Spectrogram {
    std::vector<double> t, f;   // frame centres (s), bin frequencies (Hz)
    std::vector<double> amp;    // row-major [frame][bin], bins up to fmax
    int n_t = 0, n_f = 0;
};
Spectrogram spectrogram(const double* x, int n, double fs, int win, int hop, double fmax);

// zero-phase band-pass (FFT mask with raised-cosine edges of width taper Hz); f_lo <= 0 -> low-pass, f_hi <= 0 -> high-pass
std::vector<double> bandpass(const double* x, int n, double fs, double f_lo, double f_hi, double taper = 5.0);

// complex amplitude of frequency f over x (Hann-windowed, amplitude-calibrated)
std::complex<double> dft_at(const double* x, int n, double fs, double f);

struct Peak { double f, amp; };
std::vector<Peak> find_peaks(const Spectrum& s, double fmin, double fmax, int nmax = 10, double rel = 0.02);

}  // namespace hz

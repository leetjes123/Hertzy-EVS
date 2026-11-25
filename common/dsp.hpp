// Signal processing: Butterworth filters (scipy.signal compatible), zero-phase filtering, damping fit.
#pragma once
#include <array>
#include <cmath>
#include <vector>

namespace hz::dsp {

using Sos = std::array<double, 6>;   // b0 b1 b2 a0(=1) a1 a2
using SosChain = std::vector<Sos>;

// Butterworth, even order. hp/lp <= 0 disables that side; both > 0 is a band-pass of 2*order poles (as scipy).
SosChain butter(int order, double hp, double lp, double fs);
Sos iir_notch(double f, double bw, double fs);
// notches of width bw at every multiple of mains inside [0.8*hp, upto*1.1), appended to chain
void add_mains_notches(SosChain& chain, double mains, double hp, double upto, double fs, double bw = 2.0);

// streaming filter state (2 per section)
using SosState = std::vector<std::array<double, 2>>;
void sosfilt(const SosChain& s, const float* x, float* y, int n, SosState& z);
// zero-phase, same padding and initial conditions as scipy.signal.sosfiltfilt
std::vector<double> sosfiltfilt(const SosChain& s, const std::vector<double>& x);

struct DampFit { double zeta = NAN; bool ok = false; int i0 = 0, i1 = 0; double slope = 0, icpt = 0; };
// decay of x band-passed +-10 % around f0: line through the envelope in dB, 3 to 25 dB below its maximum
DampFit damping(const std::vector<double>& x, double fs, double f0, std::vector<float>* env = nullptr);

struct DbPeak { double f = 0, db = 0; };
DbPeak interp_peak(const std::vector<float>& db, double df, int i);
// up to n prominent peaks in [fmin, fmax], at most floor dB below the strongest (scipy.signal.find_peaks rules)
std::vector<DbPeak> find_peaks_db(const std::vector<float>& db, double df, double fmin, double fmax, int distance,
                                  int n = 12, double prom = 6.0, double floor = 40.0);
// largest local value within +-rel of f; ok=false when the range is too small
bool peak_near(const std::vector<float>& db, double df, double f, DbPeak& out, double rel = 0.03);

}  // namespace hz::dsp

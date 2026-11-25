// Rotation speed from an event signal. Anything that passes a ROI once per mark (fan blade, a strip of tape, a gear
// tooth, a bolt head) makes the ROI's ON-OFF signal periodic, with lines at the repetition frequency f0 and its
// harmonics. The estimator takes the strongest line, walks down to the lowest f0 = line / m that is a real line itself
// with most of its harmonics present (so it does not lock onto 2x or 3x the true rate), refines f0 from all harmonics
// (weighted mean of f_k / k: the higher harmonics sharpen the number) and then decides what f0 is:
//   Marks = 1: f0 is the turn rate.
//   Marks = N: f0 is the pass rate (N alike marks per turn), unless every N-th harmonic clearly stands out: then f0 is
//   the turn rate itself (the marks are not alike: imbalance, one odd blade) and the passes are at N x f0.
// A bright tape mark that dominates the ROI repeats once per turn: use Marks = 1 for it.
#pragma once
#include <QString>
#include <QStringList>
#include <cmath>
#include <vector>

namespace evs {

struct RpmSettings {
    int marks = 1;              // passes per revolution: blades, teeth, or 1 for a single tape mark
    double rpmLo = 60, rpmHi = 12000;
    int harmonics = 6;          // harmonics summed per candidate
    double snrMin = 6;          // the fundamental must reach this SNR
    double win = 1.0;           // s: track frame / live window
};

struct RpmResult {
    bool ok = false;
    double rpm = NAN;
    double pass = 0;            // pass frequency p (Hz) = marks x rev/s
    double snr = 0;             // SNR of the repetition line f0
    int found = 0;              // harmonics above the noise
    int strongest = 1;          // which harmonic of f0 is the strongest line
    double shaftSnr = 0;        // marks > 1: SNR of the once-per-turn line when one was found
    double spread = 0;          // relative spread of the per-harmonic estimates (precision indicator)
    QString note;
};

// x: the ROI signal (ON-OFF per bin) at fs. The spectrum is computed here (Hann, zero padded) up to what the harmonics need.
RpmResult estimate_rpm(const std::vector<double>& x, double fs, const RpmSettings& s);
// on a finished amplitude spectrum (uniform f grid from 0); res = true resolution (1 / window length, Hz), 0 = 8 bins
RpmResult rpm_from_spectrum(const std::vector<double>& f, const std::vector<double>& amp, const RpmSettings& s, double res = 0);

struct RpmTrack {
    std::vector<double> t, rpm, snr;   // frame centres (s, from the series start + t0), NaN rpm where no estimate
};
// sliding frames of s.win seconds with 75 % overlap
RpmTrack rpm_track(const std::vector<double>& x, double fs, double t0, const RpmSettings& s);

}  // namespace evs

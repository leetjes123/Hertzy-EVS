// Measurements on a clip: per ROI signal -> spectrum -> tone (+ peaks, SNR), decay, spectrogram; onset detection;
// the measurement file format (measurement.json + results.csv + spectra.csv + signals.csv + PNGs in
// <clip>.measure/<name>/). A measurement without results is a recipe: its ROIs, window rule and settings can be applied
// to another clip or to the live stream.
#pragma once
#include <QImage>
#include <QJsonObject>
#include <QString>
#include <vector>
#include "clip.hpp"
#include "roi.hpp"
#include "rpm.hpp"

namespace evs {

struct MeasureSettings {
    double fs = 4000;                 // signal bin rate (Hz)
    QString signal = "velocity";      // velocity: the ON-OFF rate itself; displacement: integrated, detrended, band-passed
    QString window = "hann";          // hann | rect (rect + velocity = best for short pluck windows)
    double bandLo = 100, bandHi = 1000;   // band-pass (displacement) and SNR reference band
    double toneLo = 150, toneHi = 500;    // where the tone is searched
    double df = 0.25;                 // zero padding: bin spacing (Hz)
    double snrMin = 8;                // tone accepted at or above this SNR (peak / median of the band)
    double fmax = 1200;               // spectra kept up to here
    bool commonMode = true;           // drop a tone that is on most ROIs but owned by none (machine / whole-object mode)
    // time window
    QString winMode = "abs";          // abs: [t0, t1] in s from the first event; onset: [onset + skip, + len]
    double t0 = 0, t1 = 0;            // t1 <= t0 = up to the end
    double skip = 0.008, len = 0.05;
    int onset = 0;                    // which onset (0 = first)
    // spectrogram
    double sgWinMs = 40, sgHopMs = 4;
    // rotation speed per ROI (rpm.hpp): ON-OFF rate over the window, independent of the signal choice above
    bool rpmOn = false;
    int rpmMarks = 1;
    double rpmLo = 60, rpmHi = 12000;
    double rpmWin = 1.0;              // frame of the RPM-over-time track (s)
    RpmSettings rpm() const { RpmSettings r; r.marks = rpmMarks; r.rpmLo = rpmLo; r.rpmHi = rpmHi; r.snrMin = snrMin; r.win = rpmWin; return r; }

    QJsonObject toJson() const;
    static MeasureSettings fromJson(const QJsonObject& o);
};

struct TonePeak { double f = 0, snr = 0, amp = 0; };

struct RoiResult {
    int id = 0;
    QString name;
    QColor color;
    long long events = 0;
    int pixels = 0;
    std::vector<double> sig;          // the analysed signal over the window (fs)
    std::vector<double> f, amp;       // spectrum up to fmax
    double median = 0;                // noise floor of the band
    std::vector<TonePeak> peaks;      // in the tone band, strongest first
    TonePeak tone;                    // picked tone (own, not common-mode)
    bool ok = false;                  // tone with SNR >= snrMin
    double zeta = NAN, decayS = NAN;  // damping ratio and 1/e decay time of the tone (from the whole clip after the window start)
    RpmResult rpm;                    // when MeasureSettings::rpmOn
};

struct Onset { double t = 0; double pre = 0; QString source; };   // t in s from the first event, pre = rate before (ev/ms)

struct MeasureResult {
    double t0 = 0, t1 = 0;            // the window used
    std::vector<RoiResult> rois;
    std::vector<double> common;       // common-mode tones found
    QString note;
};

// plucks / taps: external trigger rising edges when present, else steep jumps of the 1 ms event rate
std::vector<Onset> find_onsets(const Clip& c, double jump = 4.0, double minGap = 0.1, const std::vector<float>* rate = nullptr);
// window [t0, t1] for the settings (onsets needed for the onset mode); false when it does not fit the clip
bool measure_window(const Clip& c, const MeasureSettings& s, const std::vector<Onset>& onsets, double& t0, double& t1, QString* why);

// spectrum of x (mean removed, window per settings, zero-padded to df), amplitude-calibrated, up to fmax
void spectrum_of(const std::vector<double>& x, const MeasureSettings& s, std::vector<double>& f, std::vector<double>& amp);
// analysed signal of a ROI over [t0, t1)
long long roi_series(const Clip& c, const Roi& r, const RoiPixels& px, const MeasureSettings& s, double t0, double t1, std::vector<double>& out);

MeasureResult run_measurement(const Clip& c, const std::vector<Roi>& rois, const MeasureSettings& s, const std::vector<Onset>& onsets);
// tone search on a list of finished spectra (also used live)
void pick_tones(std::vector<RoiResult>& rr, const MeasureSettings& s, std::vector<double>* common);

struct Spectrogram2 { std::vector<double> t, f; std::vector<float> amp; int nt = 0, nf = 0; std::vector<double> trackT, trackF; };
// spectrogram of a ROI over the whole clip, with the tone track (strongest bin in the tone band per frame)
Spectrogram2 roi_spectrogram(const Clip& c, const Roi& r, const MeasureSettings& s);
// rotation speed of a ROI over the whole clip (frames of s.rpmWin, 75 % overlap)
RpmTrack roi_rpm_track(const Clip& c, const Roi& r, const MeasureSettings& s);

// ---- files ----------------------------------------------------------------------------------------------------------
QString measure_dir(const QString& rawPath);                // <clip>.measure
QString safe_name(const QString& s);
// writes <clip>.measure/<name>/ (measurement.json, results.csv, spectra.csv, signals.csv, map.png, spectra.png)
bool save_measurement(const QString& rawPath, const QString& name, const std::vector<Roi>& rois, const MeasureSettings& s,
                      const MeasureResult& res, const QImage& map, QString* err);
struct SavedMeasurement {
    QString name, dir, created, mapKind;
    MeasureSettings settings;
    QJsonArray rois;
    QJsonArray results;               // summary per ROI (name, f, snr, amp, ok, ...)
    double t0 = 0, t1 = 0;
};
bool load_measurement(const QString& dir, SavedMeasurement& out);
std::vector<SavedMeasurement> list_measurements(const QString& rawPath);

// map with the ROI outlines drawn on it, and a simple multi-series spectrum figure (for the PNG files)
QImage draw_rois(const QImage& map, const std::vector<Roi>& rois, const std::vector<RoiResult>* res = nullptr);
QImage draw_spectra(const MeasureResult& res, const MeasureSettings& s, const QString& title, int w = 1400);

}  // namespace evs

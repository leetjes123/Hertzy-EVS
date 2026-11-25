// A decoded recording in memory, for the analysis page: packed events (8 bytes each), a 1 ms time index, event rates,
// external triggers and the sidecar metadata. Everything the analysis needs is computed from this (maps, ROI signals).
// Also: a streaming scan of a .raw file (count map + rates + triggers, no event storage) for thumbnails and stats.
#pragma once
#include <QImage>
#include <QJsonObject>
#include <QString>
#include <atomic>
#include <cstdint>
#include <functional>
#include <vector>
#include "evt3.hpp"
#include "roi.hpp"

namespace evs {

using Progress = std::function<void(double)>;   // 0..1

// one pass over a .raw file without keeping the events
struct ScanOut {
    int W = 1280, H = 720;
    uint64_t events = 0, on = 0;
    double duration = 0;                 // s, first to last event
    std::vector<float> count;            // W*H, events per pixel over the whole file
    std::vector<float> rate;             // events per 1 ms bin
    std::vector<TrigEv> triggers;        // t relative to the first event (us)
    int64_t t0 = 0;                      // absolute time of the first event (us)
};
bool scan_raw(const QString& path, ScanOut& out, QString* err, const Progress& prog = {}, const std::atomic<bool>* cancel = nullptr);

class Clip {
public:
    QString path, name;
    QJsonObject meta;                    // sidecar (.json next to the .raw), may be empty
    int W = 1280, H = 720;
    int64_t t0abs = 0;                   // us
    double duration = 0;                 // s
    std::vector<uint32_t> xyp;           // x | y << 11 | p << 22
    std::vector<uint32_t> t;             // us from the first event
    std::vector<uint32_t> msIndex;       // first event index of each 1 ms bin (size nms + 1)
    std::vector<float> rateAll, rateOn;  // events per 1 ms bin
    std::vector<TrigEv> triggers;        // t relative (us)
    double thetaOn = 0.2, thetaOff = 0.2;   // ON/OFF weights of the ON-OFF signal (bias model, relative)

    static constexpr uint64_t kMaxEvents = 450'000'000ULL;   // ~3.6 GB: refuse bigger files (Jetson has 8 GB)

    bool load(const QString& rawPath, QString* err, const Progress& prog = {}, const std::atomic<bool>* cancel = nullptr);
    size_t size() const { return t.size(); }
    static uint16_t ex(uint32_t v) { return uint16_t(v & 0x7FF); }
    static uint16_t ey(uint32_t v) { return uint16_t((v >> 11) & 0x7FF); }
    static uint8_t ep(uint32_t v) { return uint8_t((v >> 22) & 1); }
    // first event at or after s (seconds from the first event)
    size_t at(double s) const;

    // ON and OFF event counts per pixel in [t0, t1)
    void countMaps(double t0, double t1, std::vector<float>& on, std::vector<float>& off) const;
    // ROI signal at fs over [t0, t1): per bin the (signed) ON / OFF counts of the ROI pixels, combined per `kind`.
    // Returns the number of events that fell in the ROI.
    long long roiSignal(const RoiPixels& px, SignalKind kind, bool sided, double t0, double t1, double fs, std::vector<double>& out) const;
    // event rate of a set of pixels (or all when px is null), 1 ms bins over the whole clip
    std::vector<float> roiRate(const RoiPixels* px) const;
};

// ---- maps -------------------------------------------------------------------------------------------------------------
struct PixelSpectra {
    int W = 0, H = 0;
    std::vector<float> freq, amp, snr, energy;   // W*H, NaN / 0 where not computed
    double fLo = 0, fHi = 0;
    int pixels = 0;
};
// per-pixel ON-OFF spectrum over [t0, t1) for the most active pixels (at most maxPix): dominant frequency in
// [toneLo, toneHi], its amplitude and SNR (peak / median of the band) and the energy in [bandLo, bandHi]
PixelSpectra pixel_spectra(const Clip& c, double t0, double t1, double fs, double toneLo, double toneHi, double bandLo, double bandHi,
                           int maxPix, const std::atomic<bool>* cancel = nullptr);

QRgb inferno(float t);   // 0..1
QRgb turbo(float t);
QImage render_log(const std::vector<float>& v, int W, int H, float* vmax = nullptr);          // log colour map
QImage render_polarity(const std::vector<float>& on, const std::vector<float>& off, int W, int H);   // ON blue / OFF orange
QImage render_freq(const PixelSpectra& ps, double snrMin);                                     // hue = frequency
QImage render_energy(const PixelSpectra& ps);
QImage thumbnail(const std::vector<float>& count, int W, int H, int tw);                       // log map scaled to width tw

}  // namespace evs

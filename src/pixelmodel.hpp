// Approximate IMX636 pixel model and the simulated scene.
//
// !! The bias -> threshold / corner-frequency maps below are ASSUMPTIONS (rough, monotonic, inside the documented
// !! bias ranges), not a calibration of this camera. They only say "lower diff = more sensitive", "higher hpf = more
// !! low-frequency rejection", "lower fo = lower bandwidth". Replace them by a measured calibration (record a known
// !! motion) before using them for anything but the simulator.
//
// Triton2 EVS node ranges (read from the camera 2026-11-11): diff_on -88..152, diff_off -38..202, fo -43..22, hpf 0..120, refr -20..235.
#pragma once
#include <algorithm>
#include <cmath>

namespace evs {

struct BiasRange { const char* key; int lo, hi; };
inline constexpr BiasRange kBiasRanges[] = {
    {"bias_diff_on", -85, 140}, {"bias_diff_off", -35, 190}, {"bias_fo", -35, 55}, {"bias_hpf", 0, 120}, {"bias_refr", -20, 235}};

// contrast threshold in log-intensity units (0.2 ~ 22 % contrast step at the factory setting)
inline double theta_on(double diff_on) { return std::max(0.04, 0.20 * std::exp(diff_on / 106.0)); }
inline double theta_off(double diff_off) { return std::max(0.04, 0.20 * std::exp(diff_off / 106.0)); }
// pixel front-end high-pass corner (Hz): ~0.5 Hz at hpf 0, ~16 Hz at 60, ~500 Hz at 120
inline double hpf_corner_hz(double hpf) { return 0.5 * std::pow(10.0, hpf / 40.0); }
// photoreceptor / source-follower low-pass corner (Hz) at good light: ~3 kHz at fo 0
inline double fo_corner_hz(double fo) { return 3000.0 * std::pow(10.0, fo / 60.0); }
// refractory period (us): ~50 us at 0, shorter for higher refr
inline double refractory_us(double refr) { return std::clamp(50.0 * std::pow(10.0, -refr / 120.0), 0.5, 500.0); }

// gain of a first-order high-pass / low-pass at f
inline double hp_gain(double f, double fc) { const double r = f / std::max(fc, 1e-9); return r / std::sqrt(1 + r * r); }
inline double lp_gain(double f, double fc) { const double r = f / std::max(fc, 1e-9); return 1 / std::sqrt(1 + r * r); }

// Lateral motion of an edge with log contrast C = ln(I_bright / I_dark) that is spread over w px (optics + pixel):
// the steepest edge pixel sees dL = C / w per px of motion. A vibration of amplitude A (px) reaches the threshold
// theta peak-to-peak when 2 A C / w >= theta, so the smallest amplitude that makes events reliably is
inline double min_amplitude_px(double theta, double contrast_ratio, double edge_px) {
    const double c = std::log(std::max(contrast_ratio, 1.01));
    return theta * edge_px / (2 * c);
}
// The signed events of both edges of a bar, counted over L rows of a probe, give the displacement:
// n = 2 L C dx / theta  ->  dx (px) = n theta / (2 L C)   (independent of the edge blur)
inline double px_per_event(double theta, double contrast_ratio, double rows) {
    const double c = std::log(std::max(contrast_ratio, 1.01));
    return theta / (2 * std::max(rows, 1.0) * c);
}

// ---- simulated lab bench (sim_source.cpp) ----------------------------------------------------------------------------
// ringing bars in the left part of the sensor: tones (Hz) and lateral amplitudes (um) right after a tap, damping ratio
inline constexpr int kSimBars = 4;
inline constexpr double kSimBarArea = 0.5;                       // fraction of the sensor width the bars use
inline constexpr double kSimBarHz[kSimBars] = {212.40, 287.65, 335.20, 401.85};
inline constexpr double kSimBarAmpUm[kSimBars] = {33.0, 99.0, 100.0, 35.0};
inline constexpr double kSimBarZeta = 0.0015;                    // vibration decays with exp(-zeta 2 pi f t)
// fan in the right part: hub centre x (fraction of the width, at mid height), blade radius and hub radius (px)
inline constexpr double kSimFanX = 0.75, kSimFanR = 70, kSimFanHub = 18;
inline constexpr double kSimBladeDuty = 0.45;                    // blade width / blade pitch
inline constexpr double kSimBladeContrast = 2.0;                 // background / blade brightness
inline constexpr double kSimTapeFrom = 0.75, kSimTapeGain = 1.8; // tape on blade 1 beyond 75 % of the radius, 1.8x background
inline constexpr double kSimFanWander = 0.005;                   // +-0.5 % speed wander at 0.2 Hz

}  // namespace evs

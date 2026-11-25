// hz_tests: EVT3 codec, the RPM estimator, a full simulated recording (sim -> engine -> .raw -> clip -> tones / RPM),
// ROIs, the analysis chain and library operations.
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <clocale>
#include <QJsonObject>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <thread>

#include "clip.hpp"
#include "detect.hpp"
#include "library.hpp"
#include "measure.hpp"
#include "roi.hpp"
#include "rpm.hpp"
#include "engine.hpp"
#include "pixelmodel.hpp"
#include "source.hpp"

using namespace evs;
static int fails = 0, checks = 0;
#define CHECK(c, ...) do { ++checks; if (!(c)) { ++fails; std::printf("FAIL %s:%d  ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static void test_codec() {
    // round trip with a time wrap (24 bit = 16.78 s) and every polarity / row change
    std::vector<Ev> in;
    int64_t t = 16'700'000;
    for (int i = 0; i < 20000; ++i) {
        t += (i % 7 == 0) ? 900 : 3;
        in.push_back(Ev{uint16_t((i * 37) % 1280), uint16_t((i / 5) % 720), uint8_t(i % 3 == 0), t});
    }
    Evt3Encoder enc;
    std::vector<uint16_t> w;
    enc.encode(in.data(), 10000, w);
    enc.encode(in.data() + 10000, 10000, w);
    Evt3Decoder dec;
    std::vector<Ev> out;
    // feed as bytes in odd-sized pieces
    const uint8_t* b = reinterpret_cast<const uint8_t*>(w.data());
    const size_t nb = w.size() * 2;
    for (size_t o = 0; o < nb;) { const size_t k = std::min<size_t>(1237, nb - o); dec.decodeBytes(b + o, k, out); o += k; }
    CHECK(out.size() == in.size(), "decoded %zu of %zu events", out.size(), in.size());
    bool same = out.size() == in.size();
    // decoded time = input time modulo the wrap epoch the decoder saw first (the stream starts at epoch 0)
    const int64_t base = in[0].t - out[0].t;
    for (size_t i = 0; same && i < in.size(); ++i)
        same = in[i].x == out[i].x && in[i].y == out[i].y && in[i].p == out[i].p && in[i].t - out[i].t == base;
    CHECK(same, "round trip differs");
    CHECK(dec.badWords == 0, "bad words %llu", (unsigned long long)dec.badWords);

    // vectorised words as the sensor sends them: TIME_HIGH, TIME_LOW, ADDR_Y, VECT_BASE_X, VECT_12, VECT_8
    const uint16_t v[] = {0x8001, 0x6005, 0x0064, 0x3800 | 100, 0x4000 | 0b100000000101, 0x5000 | 0b10000001, 0x2000 | 7};
    std::vector<Ev> e2;
    Evt3Decoder d2;
    d2.decode(v, 7, e2);
    CHECK(e2.size() == 6, "vector words gave %zu events", e2.size());
    if (e2.size() == 6) {
        CHECK(e2[0].x == 100 && e2[1].x == 102 && e2[2].x == 111 && e2[0].p == 1, "VECT_12 x %d %d %d", e2[0].x, e2[1].x, e2[2].x);
        CHECK(e2[3].x == 112 && e2[4].x == 119, "VECT_8 x %d %d", e2[3].x, e2[4].x);
        CHECK(e2[5].x == 7 && e2[5].p == 0 && e2[5].y == 100 && e2[5].t == (1 << 12) + 5, "ADDR_X event");
    }
}

// RPM estimator on synthetic pulse trains: 3 blades + a weaker once-per-turn mark, noise; marks = 3 and marks = 1
static void test_rpm_math() {
    const double fs = 4000, rpm = 1234.5, rps = rpm / 60;
    const int n = (int)(2.0 * fs);
    std::vector<double> x(n, 0.0);
    unsigned seed = 1;
    auto rnd = [&] { seed = seed * 1103515245u + 12345u; return ((seed >> 8) & 0xFFFF) / 65535.0 - 0.5; };
    for (int i = 0; i < n; ++i) {
        const double ph = std::fmod(rps * i / fs, 1.0);           // turn phase 0..1
        const double bp = std::fmod(ph * 3, 1.0);                  // blade phase
        x[i] = (bp < 0.05 ? 4.0 : bp > 0.5 && bp < 0.55 ? -4.0 : 0.0) + (ph < 0.04 ? 3.0 : 0.0) + 2.0 * rnd();
    }
    RpmSettings s;
    s.marks = 3;
    s.rpmLo = 100;
    s.rpmHi = 6000;
    RpmResult r = estimate_rpm(x, fs, s);
    CHECK(r.ok && std::fabs(r.rpm - rpm) < 0.0005 * rpm, "3 marks: %.2f rpm vs %.2f (%s)", r.rpm, rpm, qPrintable(r.note));
    CHECK(r.shaftSnr >= 3, "3 marks: shaft line seen (SNR %.1f)", r.shaftSnr);
    s.marks = 1;
    r = estimate_rpm(x, fs, s);
    CHECK(r.ok && std::fabs(r.rpm - rpm) < 0.0005 * rpm, "1 mark: %.2f rpm vs %.2f (%s)", r.rpm, rpm, qPrintable(r.note));
    // without the mark only the blade passes repeat: with marks = 3 the speed is still right
    for (int i = 0; i < n; ++i) { const double ph = std::fmod(rps * i / fs, 1.0); if (ph < 0.04) x[i] -= 3.0; }
    s.marks = 3;
    r = estimate_rpm(x, fs, s);
    CHECK(r.ok && std::fabs(r.rpm - rpm) < 0.0005 * rpm, "3 blades, no mark: %.2f rpm (%s)", r.rpm, qPrintable(r.note));
    // speed range that excludes the true speed: no false number
    s.rpmLo = 3000;
    r = estimate_rpm(x, fs, s);
    CHECK(!r.ok || std::fabs(r.rpm - rpm) > 100, "out of range: %.1f rpm", r.rpm);
}

static void test_simulated_recording(const QString& dir) {
    // the simulated bench at 0.1 mm/px: four ringing bars on the left, the fan at 480 rpm on the right
    auto src = make_sim_source();
    Engine eng(1280, 720);
    eng.setEncoded(true);
    src->sink = [&](Chunk&& c) { eng.push(std::move(c)); };
    QString err;
    src->open(&err);
    for (auto& kv : std::vector<std::pair<const char*, double>>{{"bias_diff_on", -40}, {"bias_diff_off", -20}, {"bias_hpf", 80}, {"bias_fo", -10}})
        src->setFeature(kv.first, kv.second, nullptr);
    src->setSimParam("scale", 0.10);
    src->setSimParam("contrast", 4.0);
    src->setSimParam("tapEvery", 0.0);
    src->setSimParam("fanRpm", 480.0);
    src->start(&err);
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    const QString path = QDir(dir).filePath("sim_test.raw");
    CHECK(eng.record(path, raw_header(1280, 720), 0.2, 1.5, false, 4), "record() refused");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    src->tap();
    RecStatus rs;
    for (int k = 0; k < 600; ++k) {   // up to 30 s (the simulator may run slower than real time)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        rs = eng.recStatus();
        if (rs.finished) break;
    }
    const QVariantMap st = src->stats();
    std::printf("  sim: %.2f Mev/s, x%.2f real time; recording %.2f s, %.1f MB, %.2f M events\n", st.value("cameraRate").toDouble() / 1e6,
                st.value("realtime").toDouble(), rs.seconds, rs.bytes / 1048576.0, rs.events / 1e6);
    CHECK(rs.finished, "recording did not finish (state %d)", rs.state);
    src->stop();

    Clip c;
    CHECK(c.load(path, &err), "clip load: %s", qPrintable(err));
    CHECK(std::llabs((long long)c.size() - (long long)rs.events) <= (long long)(rs.events / 50 + 10), "file has %zu events, recorder counted %llu",
          c.size(), (unsigned long long)rs.events);
    // a line ROI along each bar: x0 = (i + 0.5) / 4 * 640, tilt (i - 1.5) * 4.5 deg about the centre row
    std::vector<Roi> bars;
    for (int i = 0; i < kSimBars; ++i) {
        const double x0 = (i + 0.5) / kSimBars * kSimBarArea * 1280, tn = std::tan((i - 1.5) * 4.5 * M_PI / 180);
        Roi r;
        r.id = i + 1;
        r.type = RoiType::Line;
        r.pts = {QPointF(x0 + (160 - 360) * tn, 160), QPointF(x0 + (560 - 360) * tn, 560)};
        r.half = 14;   // bar radius 10 px + edge
        bars.push_back(r);
    }
    const double trig = rs.tFirst >= 0 ? (rs.tTrigger - rs.tFirst) * 1e-6 : 0.2;
    MeasureSettings ms;
    ms.signal = "displacement";
    ms.toneLo = 150;
    ms.toneHi = 500;
    ms.t0 = trig + 0.15;
    ms.t1 = c.duration;
    ms.commonMode = false;
    const MeasureResult mr = run_measurement(c, bars, ms, {});
    CHECK(mr.rois.size() == bars.size(), "%zu bar results", mr.rois.size());
    for (int i = 0; i < (int)mr.rois.size(); ++i) {
        const RoiResult& r = mr.rois[i];
        std::printf("  bar %d: %.2f Hz (truth %.2f), SNR %.0f\n", i + 1, r.tone.f, kSimBarHz[i], r.tone.snr);
        CHECK(r.ok && std::fabs(r.tone.f - kSimBarHz[i]) < 0.01 * kSimBarHz[i], "bar %d tone %.2f vs %.2f", i + 1, r.tone.f, kSimBarHz[i]);
    }
    // the fan (hub at 960, 360): a small rectangle in the tape zone (beyond 52 px) repeats once per turn -> Marks = 1;
    // one closer to the hub sees three alike blades -> Marks = 3
    MeasureSettings vs;
    vs.signal = "velocity";
    auto fanSignal = [&](double x0, double x1) {
        Roi fan;
        fan.type = RoiType::Rect;
        fan.pts = {QPointF(x0, 354), QPointF(x1, 366)};
        std::vector<double> x;
        roi_series(c, fan, rasterize(fan, c.W, c.H), vs, 0, c.duration, x);
        return x;
    };
    const std::vector<double> xTape = fanSignal(1015, 1028), xBlades = fanSignal(985, 1005);
    for (int marks : {1, 3}) {
        RpmSettings rp;
        rp.marks = marks;
        rp.rpmLo = 100;
        rp.rpmHi = 3000;
        const RpmResult r = estimate_rpm(marks == 1 ? xTape : xBlades, vs.fs, rp);
        std::printf("  fan, %d mark(s): %.2f rpm (set 480 +-0.5 %%), SNR %.0f, %d harmonics  %s\n", marks, r.rpm, r.snr, r.found, qPrintable(r.note));
        CHECK(r.ok && std::fabs(r.rpm - 480) < 0.008 * 480, "fan %d mark(s): %.2f rpm", marks, r.rpm);
    }
    const std::vector<double>& x = xBlades;
    RpmSettings tp;
    tp.marks = 3;
    tp.rpmLo = 100;
    tp.rpmHi = 3000;
    tp.win = 0.5;
    const RpmTrack tr = rpm_track(x, vs.fs, 0, tp);
    int good = 0;
    for (double v : tr.rpm) good += std::isfinite(v) && std::fabs(v - 480) < 0.01 * 480;
    CHECK(tr.t.size() >= 4 && good >= (int)tr.t.size() - 1, "rpm track: %d of %zu frames at 480 rpm", good, tr.t.size());
}

// ROI rasterising: sided line signs, rectangle / polygon pixel counts, mask RLE round trip, JSON round trip
static void test_rois() {
    Roi l;
    l.type = RoiType::Line;
    l.pts = {QPointF(100, 100), QPointF(100, 200)};   // vertical, +normal points to -x
    l.half = 5;
    const RoiPixels p = rasterize(l, 1280, 720);
    int plus = 0, minus = 0;
    for (size_t i = 0; i < p.idx.size(); ++i) {
        const int x = p.idx[i] % 1280;
        if (p.sign[i] > 0) { ++plus; CHECK(x >= 100, "line: + side at x=%d (expected the -normal side, x >= 100)", x); }
        else { ++minus; CHECK(x < 100, "line: - side at x=%d", x); }
    }
    CHECK(plus > 400 && minus > 400 && std::abs(plus - minus) <= 110, "line sides %d / %d", plus, minus);
    Roi r;
    r.type = RoiType::Rect;
    r.pts = {QPointF(10, 20), QPointF(40, 30)};
    CHECK(rasterize(r, 1280, 720).idx.size() == 300, "rect has %zu px (expected 30 x 10)", rasterize(r, 1280, 720).idx.size());
    Roi g;
    g.type = RoiType::Polygon;
    g.pts = {QPointF(0, 0), QPointF(100, 0), QPointF(0, 100)};   // triangle, area 5000
    const size_t n = rasterize(g, 1280, 720).idx.size();
    CHECK(n > 4850 && n < 5150, "triangle has %zu px (area 5000)", n);
    std::vector<uint8_t> m(1280 * 720, 0);
    for (int i = 5000; i < 5100; ++i) m[i] = 1;
    m[700000] = 1;
    CHECK(mask_from_rle(mask_rle(m), m.size()) == m, "mask RLE round trip");
    Roi k;
    k.type = RoiType::Mask; k.maskW = 1280; k.maskH = 720; k.mask = m; k.name = "M1"; k.color = QColor("#ff0000"); k.id = 3;
    const Roi k2 = roi_from_json(roi_to_json(k), 1280, 720);
    CHECK(k2.mask == m && k2.name == "M1" && k2.id == 3, "mask ROI JSON round trip");
    RoiSet rs;
    rs.add(l);
    rs.add(r);
    CHECK(rs.rois[0].name == "L1" && rs.rois[1].name == "R2", "auto names %s %s", qPrintable(rs.rois[0].name), qPrintable(rs.rois[1].name));
    rs.clear();
    CHECK(rs.add(g) == 1, "ids restart after clear");
}

// the simulated recording through the analysis chain: clip, onset, line detection, measurement files, library ops
static void test_clip_pipeline(const QString& dir) {
    const QString raw = QDir(dir).filePath("sim_test.raw");
    if (!QFileInfo::exists(raw)) { CHECK(false, "no sim_test.raw"); return; }
    const QString lib = QDir(dir).filePath("lib");
    QDir(lib).removeRecursively();
    QDir().mkpath(lib + "/P1");
    const QString c1 = lib + "/P1/clip_a.raw";
    QFile::copy(raw, c1);
    QFile::copy(raw.left(raw.size() - 4) + ".json", lib + "/P1/clip_a.json");
    Clip c;
    QString err;
    CHECK(c.load(c1, &err), "clip load: %s", qPrintable(err));
    CHECK(c.size() > 1000 && c.duration > 0.5, "clip: %zu events, %.2f s", c.size(), c.duration);
    size_t bad = 0;
    for (double s : {0.0, 0.1234, 0.5, c.duration * 0.9}) {
        const size_t i = c.at(s);
        if (i < c.size() && c.t[i] < s * 1e6 - 1) ++bad;
        if (i > 0 && i <= c.size() && c.t[i - 1] >= s * 1e6 + 1) ++bad;
    }
    CHECK(bad == 0, "time index: %zu wrong", bad);
    std::vector<float> on, off;
    c.countMaps(0, c.duration + 1, on, off);
    double tot = 0;
    for (size_t i = 0; i < on.size(); ++i) tot += on[i] + off[i];
    CHECK(std::fabs(tot - c.size()) < 1, "count map %.0f vs %zu events", tot, c.size());
    const std::vector<Onset> ons = find_onsets(c);
    CHECK(!ons.empty(), "an onset (the tap) is found");
    MeasureSettings s;
    s.winMode = "onset"; s.skip = 0.01; s.len = 0.5; s.signal = "displacement";
    double w0 = 0, w1 = 0;
    CHECK(measure_window(c, s, ons, w0, w1, &err), "window: %s", qPrintable(err));
    c.countMaps(w0, w1, on, off);
    for (size_t i = 0; i < on.size(); ++i) on[i] += off[i];
    const std::vector<Roi> lines = detect_lines(on, c.W, c.H);
    CHECK(lines.size() >= 2, "%zu lines detected in the simulated scene", lines.size());
    RoiSet rs;
    for (const Roi& r : lines) rs.add(r);
    const MeasureResult res = run_measurement(c, rs.rois, s, ons);
    // the bars ring at kSimBarHz; the 26 Hz rocking puts sidebands of almost equal height next to them on a line that
    // covers one edge only, so a single line may pick a sideband: require 3 of the 4 picked and 335.2 Hz among the peaks
    int picked = 0;
    for (double ft : kSimBarHz) {
        bool hit = false;
        for (const RoiResult& r : res.rois) if (r.ok && std::fabs(r.tone.f - ft) < 0.01 * ft) hit = true;
        picked += hit;
    }
    bool got335 = false;
    for (const RoiResult& r : res.rois) for (const TonePeak& q : r.peaks) if (std::fabs(q.f - 335.2) < 0.01 * 335.2) got335 = true;
    CHECK(picked >= 3, "%d of the 4 simulated tones picked by the detected lines", picked);
    CHECK(got335, "335.2 Hz among the peaks of a detected line");
    if (picked < 3 || !got335) for (const RoiResult& r : res.rois) std::printf("   %s %.2f Hz SNR %.0f ok %d  (%s)\n", qPrintable(r.name), r.tone.f, r.tone.snr, r.ok, qPrintable(res.note));
    CHECK(save_measurement(c1, "test m", rs.rois, s, res, render_log(on, c.W, c.H), &err), "save: %s", qPrintable(err));
    const auto ms = list_measurements(c1);
    CHECK(ms.size() == 1 && ms[0].name == "test m" && ms[0].rois.size() == (int)rs.rois.size(), "measurement listed with its ROIs");
    for (const char* f : {"measurement.json", "results.csv", "spectra.csv", "signals.csv", "map.png", "spectra.png"})
        CHECK(!ms.empty() && QFileInfo::exists(QDir(ms[0].dir).filePath(f)), "measurement file %s", f);
    if (!ms.empty()) {   // the saved recipe reproduces the tones
        RoiSet r2;
        r2.fromJson(ms[0].rois, c.W, c.H);
        const MeasureResult res2 = run_measurement(c, r2.rois, ms[0].settings, ons);
        bool same = res2.rois.size() == res.rois.size();
        for (size_t i = 0; same && i < res.rois.size(); ++i) same = std::fabs(res2.rois[i].tone.f - res.rois[i].tone.f) < 0.05;
        CHECK(same, "recipe reproduces the tones");
    }
    CHECK(make_stats(c1, &err), "stats: %s", qPrintable(err));
    ClipInfo ci = clip_info(c1, "P1");
    CHECK(ci.hasStats && !ci.thumb.isEmpty() && ci.measurements == 1, "clip info: stats %d thumb %d meas %d", ci.hasStats, !ci.thumb.isEmpty(), ci.measurements);
    CHECK(std::fabs(ci.events - c.size()) < 1, "stats events %.0f", ci.events);
    CHECK(patch_sidecar(c1, QJsonObject{{"description", "hello"}}), "description");
    QString nr;
    CHECK(rename_clip(c1, "clip b", &nr, &err), "rename: %s", qPrintable(err));
    ci = clip_info(nr, "P1");
    CHECK(ci.name == "clip_b" && ci.description == "hello" && ci.measurements == 1 && !ci.thumb.isEmpty(), "renamed clip keeps its files (%s)", qPrintable(ci.name));
    Library L;
    L.root = lib;
    CHECK(L.createProject("P2", &err), "create project: %s", qPrintable(err));
    CHECK(L.projects() == QStringList({"P1", "P2"}), "projects: %s", qPrintable(L.projects().join(",")));
    QString nr2;
    CHECK(move_clip(nr, L.projectDir("P2"), &nr2, &err), "move: %s", qPrintable(err));
    CHECK(L.clipCount("P1") == 0 && L.clipCount("P2") == 1 && clip_info(nr2, "P2").measurements == 1, "moved with its measurement");
    CHECK(trash_clip(nr2, lib, &err), "trash: %s", qPrintable(err));
    CHECK(L.clipCount("P2") == 0 && QFileInfo::exists(lib + "/_trash/P2/clip_b.raw") && !L.projects().contains("_trash"), "trashed into _trash/P2");
}

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);   // the measurement PNGs draw text
    std::setlocale(LC_NUMERIC, "C");   // Qt adopts the system locale on Linux: keep printf / strtod on decimal points
    const QString dir = argc > 1 ? QString(argv[1]) : QDir::temp().filePath("hertzyevs_tests");
    QDir().mkpath(dir);
    test_codec();
    test_rpm_math();
    test_simulated_recording(dir);
    test_rois();
    test_clip_pipeline(dir);
    std::printf("%d/%d checks passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}

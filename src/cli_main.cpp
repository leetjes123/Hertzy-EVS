// hz_cli: the HertzyEVS analysis without the GUI (handy over ssh on the Jetson).
//   hz_cli info  clip.raw                    decode, print duration / events / onsets / triggers
//   hz_cli stats clip.raw [...]              write the library stats + thumbnail (like the Library page does)
//   hz_cli measure clip.raw [options]        ROIs from --lines / --blobs / --recipe DIR, then per-ROI tones
//      --recipe DIR       ROIs + settings + window rule of a saved measurement (DIR = <clip>.measure/<name>)
//      --lines | --blobs  detect ROIs on the count map of the window (default: --lines when no recipe)
//      --win abs|onset    --t0 S --t1 S  |  --skip S --len S --onset K     (default: onset, skip 0.01, len 0.59)
//      --signal velocity|displacement   --window hann|rect   --tone LO HI   --band LO HI   --snr N   --fs HZ
//      --rect X0 Y0 X1 Y1 a rectangle ROI (repeatable; replaces the detection)
//      --rpm MARKS        rotation speed per ROI (MARKS = passes per turn: 1 for a tape mark, N for N blades)
//      --rpm-range LO HI  expected speed range (rpm, default 60 12000)
//      --save NAME        save as a measurement of the clip
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <clocale>
#include <chrono>
#include <cstdio>
#include "clip.hpp"
#include "detect.hpp"
#include "library.hpp"
#include "measure.hpp"

using namespace evs;

static int usage() {
    std::printf("usage: hz_cli info|stats|measure clip.raw [options]   (see the top of cli_main.cpp)\n");
    return 2;
}

int main(int argc, char** argv) {
#ifndef _WIN32
    // QPainter text in the PNGs needs a GUI application; over ssh there is no display
    if (qEnvironmentVariableIsEmpty("DISPLAY") && qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY") && qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
#endif
    QGuiApplication app(argc, argv);
    std::setlocale(LC_NUMERIC, "C");   // Qt adopts the system locale on Linux: keep printf / strtod on decimal points
    const QStringList a = app.arguments();
    if (a.size() < 3) return usage();
    const QString cmd = a[1];
    if (cmd == "stats") {
        for (int i = 2; i < a.size(); ++i) {
            QString err;
            const auto t0 = std::chrono::steady_clock::now();
            const bool ok = make_stats(a[i], &err);
            const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            std::printf("%s  %s  %.2f s\n", qPrintable(QFileInfo(a[i]).fileName()), ok ? "ok" : qPrintable(err), s);
        }
        return 0;
    }
    const QString raw = a[2];
    Clip c;
    QString err;
    auto t0 = std::chrono::steady_clock::now();
    if (!c.load(raw, &err)) { std::printf("error: %s\n", qPrintable(err)); return 1; }
    const double tl = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const std::vector<Onset> on = find_onsets(c);
    std::printf("%s: %.3f s, %.2f M events, %zu triggers, %zu onsets, load %.2f s\n", qPrintable(c.name), c.duration, c.size() / 1e6,
                c.triggers.size(), on.size(), tl);
    for (size_t i = 0; i < on.size() && i < 20; ++i) std::printf("   onset %zu: %.4f s (%s, pre %.1f ev/ms)\n", i + 1, on[i].t, qPrintable(on[i].source), on[i].pre);
    if (cmd == "info") return 0;
    if (cmd != "measure") return usage();

    MeasureSettings s;
    s.winMode = "onset";
    s.skip = 0.01;
    s.len = 0.59;
    s.signal = "displacement";
    QString recipe, save, det;
    std::vector<Roi> rects;
    for (int i = 3; i < a.size(); ++i) {
        const QString k = a[i];
        auto nx = [&]() { return i + 1 < a.size() ? a[++i] : QString(); };
        if (k == "--recipe") recipe = nx();
        else if (k == "--lines") det = "lines";
        else if (k == "--blobs") det = "blobs";
        else if (k == "--win") s.winMode = nx();
        else if (k == "--t0") s.t0 = nx().toDouble();
        else if (k == "--t1") s.t1 = nx().toDouble();
        else if (k == "--skip") s.skip = nx().toDouble();
        else if (k == "--len") s.len = nx().toDouble();
        else if (k == "--onset") s.onset = nx().toInt() - 1;
        else if (k == "--signal") s.signal = nx();
        else if (k == "--window") s.window = nx();
        else if (k == "--tone") { s.toneLo = nx().toDouble(); s.toneHi = nx().toDouble(); }
        else if (k == "--band") { s.bandLo = nx().toDouble(); s.bandHi = nx().toDouble(); }
        else if (k == "--snr") s.snrMin = nx().toDouble();
        else if (k == "--fs") s.fs = nx().toDouble();
        else if (k == "--save") save = nx();
        else if (k == "--rpm") { s.rpmOn = true; s.rpmMarks = std::max(1, nx().toInt()); }
        else if (k == "--rpm-range") { s.rpmLo = nx().toDouble(); s.rpmHi = nx().toDouble(); }
        else if (k == "--rect") {
            Roi r;
            r.type = RoiType::Rect;
            const double x0 = nx().toDouble(), y0 = nx().toDouble(), x1 = nx().toDouble(), y1 = nx().toDouble();
            r.pts = {QPointF(x0, y0), QPointF(x1, y1)};
            rects.push_back(r);
        }
    }
    RoiSet rs;
    for (const Roi& r : rects) rs.add(r);
    const bool rpmCli = s.rpmOn;
    const int rpmMarks = s.rpmMarks;
    const double rpmLo = s.rpmLo, rpmHi = s.rpmHi;
    if (!recipe.isEmpty()) {
        SavedMeasurement m;
        if (!load_measurement(recipe, m)) { std::printf("error: no measurement.json in %s\n", qPrintable(recipe)); return 1; }
        s = m.settings;
        rs.fromJson(m.rois, c.W, c.H);
        if (rpmCli) { s.rpmOn = true; s.rpmMarks = rpmMarks; s.rpmLo = rpmLo; s.rpmHi = rpmHi; }   // --rpm overrides the recipe
    }
    double w0 = 0, w1 = 0;
    if (!measure_window(c, s, on, w0, w1, &err)) { std::printf("error: %s\n", qPrintable(err)); return 1; }
    std::vector<float> mon, moff;
    c.countMaps(w0 - s.skip, w1, mon, moff);
    for (size_t i = 0; i < mon.size(); ++i) mon[i] += moff[i];
    if (rs.rois.empty() || !det.isEmpty()) {
        t0 = std::chrono::steady_clock::now();
        const std::vector<Roi> found = det == "blobs" ? detect_blobs(mon, c.W, c.H) : detect_lines(mon, c.W, c.H);
        std::printf("detected %zu %s in %.2f s\n", found.size(), det == "blobs" ? "blobs" : "lines",
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        for (const Roi& r : found) rs.add(r);
    }
    t0 = std::chrono::steady_clock::now();
    const MeasureResult res = run_measurement(c, rs.rois, s, on);
    std::printf("window %.4f .. %.4f s, %s / %s, measured in %.3f s%s\n", res.t0, res.t1, qPrintable(s.signal), qPrintable(s.window),
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), res.note.isEmpty() ? "" : qPrintable("  (" + res.note + ")"));
    for (size_t k = 0; k < res.rois.size(); ++k) {
        const RoiResult& r = res.rois[k];
        const Roi& g = rs.rois[k];
        QString pts;
        for (const QPointF& p : g.pts) pts += QString("(%1,%2)").arg(p.x(), 0, 'f', 0).arg(p.y(), 0, 'f', 0);
        QString pk;
        for (size_t j = 0; j < r.peaks.size() && j < 4; ++j) pk += QString(" %1/%2").arg(r.peaks[j].f, 0, 'f', 1).arg(r.peaks[j].snr, 0, 'f', 0);
        std::printf("   %-5s %-26s w %5.1f ev %8lld  %s  %s\n", qPrintable(r.name), qPrintable(pts), 2 * g.half, r.events,
                    r.ok ? qPrintable(QString("%1 Hz SNR %2").arg(r.tone.f, 7, 'f', 2).arg(r.tone.snr, 5, 'f', 0)) : "   no tone          ",
                    qPrintable(pk));
        if (s.rpmOn)
            std::printf("         speed: %s\n", r.rpm.ok ? qPrintable(QString("%1 rpm (passes %2 Hz, SNR %3, %4 harmonics)%5").arg(r.rpm.rpm, 0, 'f', 2)
                                                             .arg(r.rpm.pass, 0, 'f', 3).arg(r.rpm.snr, 0, 'f', 0).arg(r.rpm.found)
                                                             .arg(r.rpm.note.isEmpty() ? QString() : "  " + r.rpm.note))
                                                  : qPrintable("none  " + r.rpm.note));
    }
    if (!res.common.empty()) { std::printf("common mode:"); for (double f : res.common) std::printf(" %.1f", f); std::printf("\n"); }
    if (!save.isEmpty()) {
        const QImage map = render_log(mon, c.W, c.H);
        if (!save_measurement(raw, save, rs.rois, s, res, map, &err)) { std::printf("error: %s\n", qPrintable(err)); return 1; }
        std::printf("saved in %s\n", qPrintable(QDir(measure_dir(raw)).filePath(safe_name(save))));
    }
    return 0;
}

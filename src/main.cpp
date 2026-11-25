// HertzyEVS: event-camera frequency and rotation-speed workbench for the LUCID Triton2 EVS (IMX636), HvA Maintenance Lab -
// record, browse and analyse on the same machine.
//   hertzyevs [--sim | --camera] [--out LIBRARY_DIR]
//   Scripted run (visual checks, tests):
//   hertzyevs --sim --shot out.png [--page library|live|analyse|camera] [--size 1920x1080] [--set key=value]...
//            [--ana key=value]... [--project NAME] [--open clip.raw] [--select clip.raw] [--roi x0,y0,x1,y1]...
//            [--detect lines|blobs] [--record S] [--wait S]
//     --open: load the clip on the analysis page; --select: select it in the library; --roi: a line ROI (live page, or the
//     analysis page with --open); --record S: record S seconds after --wait, then show --page and grab
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QPalette>
#include <cstdio>
#include <QJsonDocument>
#include <QMouseEvent>
#include <functional>
#include <memory>
#include <QGuiApplication>
#include <clocale>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSettings>
#include <QStandardPaths>
#include <QStyleHints>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <QElapsedTimer>
#include "prof.hpp"
#include "player.hpp"
#include <algorithm>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")
#else
#include <sys/resource.h>
#endif
#include <QDeadlineTimer>
#include "analyse.hpp"
#include "controller.hpp"
#include "libctl.hpp"

static QString defaultRoot() {
    // portable bundle (unzipped package): a recordings folder next to the app or one level up wins over saved settings,
    // so a moved or re-unzipped copy always opens its own recordings
    const QDir exe(QCoreApplication::applicationDirPath());
    for (const QString& rel : {QStringLiteral("recordings"), QStringLiteral("../recordings")})
        if (exe.exists(rel)) return QDir::cleanPath(exe.absoluteFilePath(rel));
    const QString saved = QSettings().value("evs/root").toString();
    if (!saved.isEmpty() && QDir(saved).exists()) return saved;
    return QDir::home().filePath("HertzyEVS");
}

static QVariant parse_value(const QString& v) {
    bool num = false;
    const double x = v.toDouble(&num);
    return v == "true" ? QVariant(true) : v == "false" ? QVariant(false) : num ? QVariant(x) : QVariant(v);
}

// CPU time of the whole process (all threads), seconds
static double process_cpu_s() {
#ifdef _WIN32
    FILETIME c, e, k, u;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return 0;
    auto s = [](FILETIME f) { return (double(f.dwHighDateTime) * 4294967296.0 + f.dwLowDateTime) * 1e-7; };
    return s(k) + s(u);
#else
    rusage r{};
    getrusage(RUSAGE_SELF, &r);
    return r.ru_utime.tv_sec + r.ru_stime.tv_sec + (r.ru_utime.tv_usec + r.ru_stime.tv_usec) * 1e-6;
#endif
}

int main(int argc, char** argv) {
#ifdef _WIN32
    timeBeginPeriod(1);   // 1 ms sleeps for the player / analysis threads (the default is 15.6 ms)
#endif
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
    QGuiApplication app(argc, argv);
    std::setlocale(LC_NUMERIC, "C");   // Qt adopts the system locale on Linux: keep printf / strtod on decimal points
    app.setApplicationName("HertzyEVS");
    app.setOrganizationName("HvA Maintenance Lab");
    // Fusion on every platform: compact, flat controls that take the dark palette below (the EVS docked look, evs/qml/Theme.qml)
    QQuickStyle::setStyle("Fusion");
    {
        // Fusion takes its colours from the palette, and without a dark desktop theme (or offscreen) that palette is light:
        // white fields with the app's light text. Set the app's dark colours explicitly.
        QPalette p;
        const QColor bg("#0c0d0f"), panel("#1c1d20"), field("#131417"), button("#2c2e33"), text("#e6e7ea"), dim("#6c717a"), accent("#4c9bff");
        p.setColor(QPalette::Window, panel);
        p.setColor(QPalette::WindowText, text);
        p.setColor(QPalette::Base, field);
        p.setColor(QPalette::AlternateBase, panel);
        p.setColor(QPalette::Text, text);
        p.setColor(QPalette::Button, button);
        p.setColor(QPalette::ButtonText, text);
        p.setColor(QPalette::BrightText, Qt::white);
        p.setColor(QPalette::PlaceholderText, dim);
        p.setColor(QPalette::Highlight, accent);
        p.setColor(QPalette::HighlightedText, Qt::white);
        p.setColor(QPalette::ToolTipBase, panel);
        p.setColor(QPalette::ToolTipText, text);
        p.setColor(QPalette::Light, QColor("#3a3d44"));
        p.setColor(QPalette::Midlight, QColor("#33363c"));
        p.setColor(QPalette::Mid, QColor("#2a2c31"));
        p.setColor(QPalette::Dark, bg);
        p.setColor(QPalette::Shadow, Qt::black);
        p.setColor(QPalette::Disabled, QPalette::Text, dim);
        p.setColor(QPalette::Disabled, QPalette::ButtonText, dim);
        p.setColor(QPalette::Disabled, QPalette::WindowText, dim);
        QGuiApplication::setPalette(p);
    }

    app.styleHints()->setColorScheme(Qt::ColorScheme::Dark);
#ifdef Q_OS_WIN
    QFont font("Segoe UI Variable Text");
#else
    QFont font("Noto Sans");   // Jetson: sudo apt install fonts-noto-core (Qt falls back to a default if missing)
#endif
    font.setPointSizeF(9.5);
    app.setFont(font);

    QString source, out, shot, page = "library", dump, open, select, project, detect, batch, mouse, dumpRois, bench;
    QStringList sets, anaSets, rois, playerSets;
    double recordS = 0, waitS = 3.0, scrollF = 0.0;
    QSize size(1920, 1080);
    const QStringList args = app.arguments();
    for (int k = 1; k < args.size(); ++k) {
        const QString& a = args[k];
        auto next = [&]() { return k + 1 < args.size() ? args[++k] : QString(); };
        if (a == "--sim") {
#ifndef EVS_NO_SIM
            source = "sim";
#endif
        }
        else if (a == "--camera") source = "camera";
        else if (a == "--out") out = next();
        else if (a == "--shot") shot = next();
        else if (a == "--page") page = next();
        else if (a == "--set") sets << next();
        else if (a == "--ana") anaSets << next();
        else if (a == "--player") playerSets << next();   // scripted: player setting key=value (play=1 starts playback)
        else if (a == "--roi" || a == "--probe") rois << next();
        else if (a == "--open") open = next();
        else if (a == "--select") select = next();
        else if (a == "--project") project = next();
        else if (a == "--detect") detect = next();
        else if (a == "--batch") batch = next();
        else if (a == "--mouse") mouse = next();
        else if (a == "--dump-rois") dumpRois = next();   // scripted: tick all clips of --project and run this recipe
        else if (a == "--record") recordS = next().toDouble();
        else if (a == "--wait") waitS = next().toDouble();
        else if (a == "--dump-nodes") dump = next();
        else if (a == "--bench") bench = next();   // scripted: run --page for --wait s, write frame / GUI-latency / CPU figures
        else if (a == "--scroll") scrollF = next().toDouble();   // scripted: scroll the side panel (0..1)
        else if (a == "--size") { const QStringList p = next().split('x'); if (p.size() == 2) size = QSize(p[0].toInt(), p[1].toInt()); }
    }
    if (!bench.isEmpty() && shot.isEmpty()) shot = bench + ".png";
    if (!shot.isEmpty()) {
        app.setOrganizationName("HvA Maintenance Lab (scripted)");   // keep the user's saved settings out of tests
#ifndef EVS_NO_SIM
        if (source.isEmpty()) source = "sim";
#endif
    }

    const QString root = out.isEmpty() ? defaultRoot() : QDir(out).absolutePath();
    evs::EvsController ctl(source);
    evs::LibraryController lib(root);
    evs::AnalyseController ana;
    evs::Player player(&ana);
    QObject::connect(&ana, &evs::AnalyseController::clipChanged, &player, [&] { player.setClip(ana.clipPtr()); });
    auto pushRois = [&] { player.setRois(ana.roiSet().rois, ana.measureSettings()); };
    QObject::connect(&ana, &evs::RoiController::roisChanged, &player, pushRois);
    QObject::connect(&ana, &evs::AnalyseController::cfgChanged, &player, pushRois);
    QObject::connect(&ana, &evs::AnalyseController::stateChanged, &player, [&] {
        const QVariantMap st = ana.state();
        player.setWindow(st.value("t0").toDouble(), st.value("t1").toDouble(), st.value("winOk").toBool());
    });
    ctl.setLibraryRoot(lib.root());
    QObject::connect(&lib, &evs::LibraryController::rootChanged, &ctl, [&] { ctl.setLibraryRoot(lib.root()); });
    QObject::connect(&ctl, &evs::EvsController::recordingSaved, &lib, [&](const QString& p) { lib.clipChanged(p); });
    QObject::connect(&ana, &evs::AnalyseController::measurementSaved, &lib, [&](const QString& p) { lib.clipChanged(p); });
    QObject::connect(&lib, &evs::LibraryController::openInAnalyse, &ana, [&](const QString& p) { ana.open(p); ctl.setv("page", "analyse"); });

    if (!dump.isEmpty()) {   // stream for --wait s, then write every camera node, the feature map and the live stats; exit
        for (const QString& kv : sets) {
            const int eq = kv.indexOf('=');
            if (eq > 0) ctl.setv(kv.left(eq), parse_value(kv.mid(eq + 1)));
        }
        const QDeadlineTimer until(int(waitS * 1000));
        while (!until.hasExpired()) { QCoreApplication::processEvents(QEventLoop::AllEvents, 50); QThread::msleep(20); }
        QFile f(dump);
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream ts(&f);
            const char* nl = "\n";
            const char* tab = "\t";
            ts << "# " << ctl.live().value("source").toString() << nl << "# feature map" << nl;
            for (const QVariant& v : ctl.features()) { const QVariantMap m = v.toMap(); ts << m["key"].toString() << " -> " << m["node"].toString() << " = " << m["value"].toString() << "  " << m["error"].toString() << nl; }
            ts << "# live" << nl;
            const QVariantMap lv = ctl.live();
            for (auto it = lv.begin(); it != lv.end(); ++it) ts << it.key() << " = " << it.value().toString() << nl;
            ts << "# nodes" << nl;
            for (const QVariant& v : ctl.nodeList("")) {
                const QVariantMap m = v.toMap();
                ts << m["name"].toString() << tab << m["type"].toString() << tab << m["value"].toString() << tab << (m["writable"].toBool() ? "rw" : "ro")
                   << tab << m["min"].toString() << ".." << m["max"].toString() << tab << m["entries"].toStringList().join("|") << tab << m["tip"].toString().simplified() << nl;
            }
        }
        return 0;
    }
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("app", &ctl);
    engine.rootContext()->setContextProperty("lib", &lib);
    engine.rootContext()->setContextProperty("ana", &ana);
    engine.rootContext()->setContextProperty("player", &player);
    engine.rootContext()->setContextProperty("scripted", !shot.isEmpty());
    engine.rootContext()->setContextProperty("edition", QStringLiteral(EVS_EDITION));   // dev | full | viewer (CMake WE_EDITION)
    engine.rootContext()->setContextProperty("startSize", size);
    engine.rootContext()->setContextProperty("startScroll", scrollF);
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app, [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.loadFromModule("HertzyEvs", "Main");
    if (engine.rootObjects().isEmpty()) return 1;
    auto* win = qobject_cast<QQuickWindow*>(engine.rootObjects().first());

    if (!shot.isEmpty() && win) {
        for (const QString& kv : sets) {
            const int eq = kv.indexOf('=');
            if (eq <= 0) continue;
            const QString key = kv.left(eq), v = kv.mid(eq + 1);
            if (key == "preset") { ctl.applyPreset(v); continue; }
            ctl.setv(key, parse_value(v));
        }
        for (const QString& kv : anaSets) {
            const int eq = kv.indexOf('=');
            if (eq > 0) ana.setCfg(kv.left(eq), parse_value(kv.mid(eq + 1)));
        }
        for (const QString& kv : playerSets) {
            const int eq = kv.indexOf('=');
            if (eq <= 0) continue;
            if (kv.left(eq) == "play") QTimer::singleShot(int(waitS * 400), &app, [&] { player.play(); });
            else if (kv.left(eq) == "export") QTimer::singleShot(int(waitS * 300), &app, [&] { player.exportVideo(30); });
            else if (kv.left(eq) == "seek") { const double v = kv.mid(eq + 1).toDouble(); QTimer::singleShot(int(waitS * 400), &app, [&player, v] { player.seek(v); }); }
            else player.setv(kv.left(eq), parse_value(kv.mid(eq + 1)));
        }
        if (!project.isEmpty()) { lib.setProject(project); ctl.setv("project", project); }
        if (!select.isEmpty()) lib.select(QFileInfo(select).absoluteFilePath());
        if (!batch.isEmpty()) { lib.checkAll(true); lib.runBatch(QFileInfo(batch).absoluteFilePath(), "batch_test"); }
        auto addRois = [&](evs::RoiController& rc) {
            if (!rois.isEmpty()) rc.clearRois();
            for (const QString& p : rois) {
                const QStringList c = p.split(',');
                if (c.size() >= 4)
                    rc.addRoi(c.size() >= 5 && c[4] == "rect" ? "rect" : "line",
                              {QVariantMap{{"x", c[0].toDouble()}, {"y", c[1].toDouble()}}, QVariantMap{{"x", c[2].toDouble()}, {"y", c[3].toDouble()}}});
            }
        };
        if (!open.isEmpty()) {
            ana.open(QFileInfo(open).absoluteFilePath());
            addRois(ana);
            if (!detect.isEmpty()) QTimer::singleShot(int(waitS * 500), &app, [&] { ana.detect(detect, true); });
        } else {
            addRois(ctl);
        }
        auto grab = [&] {
            QImage img = win->grabWindow();
            const QFileInfo fi(shot);
            QDir().mkpath(fi.absolutePath());
            img.save(shot);
            QCoreApplication::exit(img.isNull() ? 2 : 0);
        };
        if (recordS > 0) {
            ctl.setv("recLen", recordS);
            ctl.setv("page", "live");
            QObject::connect(&ctl, &evs::EvsController::recordingSaved, &app, [&](const QString&) {
                ctl.setv("page", page);
                QTimer::singleShot(1500, &app, grab);
            });
            QTimer::singleShot(int(waitS * 1000), &app, [&] { ctl.recordNow(); QTimer::singleShot(150, &app, [&] { ctl.simTap(); }); });
            QTimer::singleShot(int((waitS + recordS + 30) * 1000), &app, [] { QCoreApplication::exit(3); });
        } else if (!mouse.isEmpty()) {
            // --mouse "tool:line;drag:x0,y0,x1,y1;click:x,y;dbl:x,y;rclick:x,y;brush:x0,y0,x1,y1" in sensor pixels: real
            // mouse events through the window (RoiLayer), started after --wait; ROIs dumped to --dump-rois, then the grab
            ctl.setv("page", page);
            const bool live = page == "live";
            evs::RoiController* rc = live ? static_cast<evs::RoiController*>(&ctl) : static_cast<evs::RoiController*>(&ana);
            auto scenePt = [&, live](double x, double y) {
                // Loader-made pages are found through the visual item tree, not the QObject tree
                std::function<QQuickItem*(QQuickItem*)> find = [&](QQuickItem* it) -> QQuickItem* {
                    if (it->objectName() == (live ? "liveView" : "anaView")) return it;
                    for (QQuickItem* c : it->childItems()) if (QQuickItem* r = find(c)) return r;
                    return nullptr;
                };
                QQuickItem* v = find(win->contentItem());
                QPointF p;
                if (v) QMetaObject::invokeMethod(v, "toItem", Q_RETURN_ARG(QPointF, p), Q_ARG(double, x), Q_ARG(double, y));
                const QPointF sp = v ? v->mapToScene(p) : QPointF(-1, -1);
                if (!v) std::fprintf(stderr, "mouse: the %s view was not found\n", live ? "live" : "analysis");
                return sp;
            };
            auto send = [&](QEvent::Type t, QPointF s, Qt::MouseButton b, Qt::MouseButtons bs) {
                QMouseEvent ev(t, s, s, win->mapToGlobal(s), b, bs, Qt::NoModifier);
                QCoreApplication::sendEvent(win, &ev);
            };
            auto steps = std::make_shared<std::vector<std::function<void()>>>();
            for (const QString& st : mouse.split(';', Qt::SkipEmptyParts)) {
                const int c = st.indexOf(':');
                const QString op = st.left(c);
                const QStringList v = st.mid(c + 1).split(',');
                if (op == "tool") { const QString t = v.value(0); steps->push_back([&, t, live] { if (live) ctl.setv("tool", t); else ana.setCfg("tool", t); }); continue; }
                const double x0 = v.value(0).toDouble(), y0 = v.value(1).toDouble(), x1 = v.value(2).toDouble(), y1 = v.value(3).toDouble();
                if (op == "drag" || op == "brush") {
                    steps->push_back([=] { send(QEvent::MouseButtonPress, scenePt(x0, y0), Qt::LeftButton, Qt::LeftButton); });
                    for (int k = 1; k <= 12; ++k)
                        steps->push_back([=] { send(QEvent::MouseMove, scenePt(x0 + (x1 - x0) * k / 12, y0 + (y1 - y0) * k / 12), Qt::NoButton, Qt::LeftButton); });
                    steps->push_back([=] { send(QEvent::MouseButtonRelease, scenePt(x1, y1), Qt::LeftButton, Qt::NoButton); });
                } else if (op == "click" || op == "rclick") {
                    const Qt::MouseButton bt = op == "click" ? Qt::LeftButton : Qt::RightButton;
                    steps->push_back([=] { send(QEvent::MouseMove, scenePt(x0, y0), Qt::NoButton, Qt::NoButton); });
                    steps->push_back([=] { send(QEvent::MouseButtonPress, scenePt(x0, y0), bt, bt); });
                    steps->push_back([=] { send(QEvent::MouseButtonRelease, scenePt(x0, y0), bt, Qt::NoButton); });
                } else if (op == "dbl") {
                    steps->push_back([=] {
                        const QPointF s = scenePt(x0, y0);
                        send(QEvent::MouseButtonPress, s, Qt::LeftButton, Qt::LeftButton);
                        send(QEvent::MouseButtonRelease, s, Qt::LeftButton, Qt::NoButton);
                        send(QEvent::MouseButtonPress, s, Qt::LeftButton, Qt::LeftButton);
                        send(QEvent::MouseButtonDblClick, s, Qt::LeftButton, Qt::LeftButton);
                        send(QEvent::MouseButtonRelease, s, Qt::LeftButton, Qt::NoButton);
                    });
                }
                steps->push_back([] {});   // a pause between gestures
            }
            const int n = (int)steps->size();
            for (int k = 0; k < n; ++k) QTimer::singleShot(int(waitS * 1000) + 60 * k, &app, [steps, k] { (*steps)[k](); });
            QTimer::singleShot(int(waitS * 1000) + 60 * n + 1500, &app, [&, rc] {
                if (!dumpRois.isEmpty()) {
                    QFile f(dumpRois);
                    if (f.open(QIODevice::WriteOnly)) f.write(QJsonDocument(rc->roiSet().toJson()).toJson());
                    QFile g(dumpRois + ".results.json");   // analysis page: the per-ROI results shown in the table
                    if (!live && g.open(QIODevice::WriteOnly)) g.write(QJsonDocument::fromVariant(QVariant(ana.results())).toJson());
                }
                grab();
            });
        } else if (!bench.isEmpty()) {
            ctl.setv("page", page);
            // 1.5 s warm-up, then measure for --wait s: frame intervals, GUI-thread latency (a 2 ms timer's lateness), CPU
            auto frames = std::make_shared<std::vector<double>>(), lat = std::make_shared<std::vector<double>>();
            auto clk = std::make_shared<QElapsedTimer>();
            auto lastF = std::make_shared<double>(-1), lastT = std::make_shared<double>(-1);
            auto on = std::make_shared<bool>(false);
            auto cpu0 = std::make_shared<double>(0);
            QObject::connect(win, &QQuickWindow::frameSwapped, &app, [=] {
                if (!*on) return;
                const double t = clk->nsecsElapsed() * 1e-6;
                if (*lastF >= 0) frames->push_back(t - *lastF);
                *lastF = t;
            }, Qt::DirectConnection);
            auto probe = new QTimer(&app);
            probe->setTimerType(Qt::PreciseTimer);
            QObject::connect(probe, &QTimer::timeout, &app, [=] {
                const double t = clk->nsecsElapsed() * 1e-6;
                if (*on && *lastT >= 0) lat->push_back(std::max(0.0, t - *lastT - 2.0));
                *lastT = t;
            });
            QTimer::singleShot(1500, &app, [=] { clk->start(); *on = true; *cpu0 = process_cpu_s(); probe->start(2); hz::prof_take(); });
            QTimer::singleShot(1500 + int(waitS * 1000), &app, [=, &ctl] {
                *on = false;
                const double wall = clk->nsecsElapsed() * 1e-9, cpu = process_cpu_s() - *cpu0;
                auto pct = [](std::vector<double> v, double q) { if (v.empty()) return 0.0; std::sort(v.begin(), v.end()); return v[std::min(v.size() - 1, (size_t)(q * v.size()))]; };
                QFile f(bench);
                if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
                    QTextStream ts(&f);
                    ts << "rois " << ctl.roiSet().rois.size() << "  seconds " << wall << Qt::endl;
                    ts << "fps " << frames->size() / wall << "  frame ms p50 " << pct(*frames, 0.5) << " p95 " << pct(*frames, 0.95) << " max " << pct(*frames, 1.0) << Qt::endl;
                    ts << "gui latency ms p50 " << pct(*lat, 0.5) << " p99 " << pct(*lat, 0.99) << " max " << pct(*lat, 1.0) << Qt::endl;
                    ts << "cpu " << 100 * cpu / wall << " % of one core" << Qt::endl;
                    ts << "stage                     calls/s    ms/call    max ms   % of one core" << Qt::endl;
                    for (const auto& [name, st] : hz::prof_take())
                        ts << QString::fromStdString(name).leftJustified(24) << QString::number(st.n / wall, 'f', 1).rightJustified(9)
                           << QString::number(st.ms / std::max(1LL, st.n), 'f', 3).rightJustified(11) << QString::number(st.maxMs, 'f', 2).rightJustified(10)
                           << QString::number(st.ms / 10 / wall, 'f', 1).rightJustified(14) << Qt::endl;
                }
                QCoreApplication::exit(0);
            });
        } else {
            ctl.setv("page", page);
            QTimer::singleShot(int(waitS * 1000), &app, grab);
        }
    }
    return app.exec();
}

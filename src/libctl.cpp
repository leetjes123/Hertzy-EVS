#include "libctl.hpp"
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSettings>
#include <QTextStream>
#include <QUrl>
#include <algorithm>
#include <chrono>
#include <cmath>
#include "measure.hpp"

namespace evs {

LibraryController::LibraryController(const QString& root, QObject* parent) : QObject(parent) {
    lib_.root = root;
    QDir().mkpath(root);
    QSettings s;
    project_ = s.value("evs/lib/project").toString();
    sort_ = s.value("evs/lib/sort", "date").toString();
    batch_ = QVariantMap{{"running", false}};
    coalesce_.setSingleShot(true);
    coalesce_.setInterval(250);
    connect(&coalesce_, &QTimer::timeout, this, [this] { applyView(); updateDetail(); emit statsChanged(); });
    sth_ = std::thread([this] { statsWorker(); });
    scanProjects();
    if (!project_.isEmpty() && !lib_.projects().contains(project_)) project_.clear();
    if (project_.isEmpty() && lib_.clipCount("") == 0 && !lib_.projects().isEmpty()) project_ = lib_.projects().front();
    loadClips();
}

LibraryController::~LibraryController() {
    {
        std::lock_guard<std::mutex> lk(smx_);
        squit_ = true;
        scancel_ = true;
    }
    scv_.notify_all();
    if (sth_.joinable()) sth_.join();
    bcancel_ = true;
    if (bth_.joinable()) bth_.join();
}

// ---- projects ------------------------------------------------------------------------------------------------------------------
void LibraryController::setRoot(const QString& path) {
    if (path.isEmpty() || QDir(path).absolutePath() == QDir(lib_.root).absolutePath()) return;
    lib_.root = QDir(path).absolutePath();
    QDir().mkpath(lib_.root);
    QSettings().setValue("evs/root", lib_.root);
    project_.clear();
    selected_.clear();
    checked_.clear();
    scanProjects();
    if (lib_.clipCount("") == 0 && !lib_.projects().isEmpty()) project_ = lib_.projects().front();
    loadClips();
    emit rootChanged();
    emit projectChanged();
    emit checkedChanged();
}

void LibraryController::scanProjects() {
    projects_.clear();
    const int nUnsorted = lib_.clipCount("");
    projects_ << QVariantMap{{"name", ""}, {"label", "Unsorted"}, {"count", nUnsorted}};
    for (const QString& p : lib_.projects()) projects_ << QVariantMap{{"name", p}, {"label", p}, {"count", lib_.clipCount(p)}};
    emit projectsChanged();
}

void LibraryController::setProject(const QString& name) {
    if (name == project_) return;
    project_ = name;
    QSettings().setValue("evs/lib/project", project_);
    selected_.clear();
    checked_.clear();
    loadClips();
    emit projectChanged();
    emit checkedChanged();
}

void LibraryController::refresh() {
    scanProjects();
    loadClips();
}

bool LibraryController::createProject(const QString& name) {
    QString err;
    if (!lib_.createProject(name, &err)) { emit toast(err); return false; }
    scanProjects();
    QString n = name.trimmed();
    n.replace(QRegularExpression("[^A-Za-z0-9_\\- ]"), "_");
    setProject(n);
    return true;
}

void LibraryController::setProjectDescription(const QString& text) {
    lib_.setProjectDescription(project_, text);
    emit projectChanged();
}

// ---- clips -----------------------------------------------------------------------------------------------------------------------
void LibraryController::loadClips() {
    all_ = lib_.clips(project_);
    for (const ClipInfo& c : all_) if (!c.hasStats || c.thumb.isEmpty()) queueStats(c.raw);
    if (!selected_.isEmpty() && std::none_of(all_.begin(), all_.end(), [&](const ClipInfo& c) { return c.raw == selected_; })) selected_.clear();
    checked_.erase(std::remove_if(checked_.begin(), checked_.end(), [&](const QString& p) {
        return std::none_of(all_.begin(), all_.end(), [&](const ClipInfo& c) { return c.raw == p; }); }), checked_.end());
    applyView();
    updateDetail();
}

QVariantMap LibraryController::toVariant(const ClipInfo& c) const {
    QString thumb;
    if (!c.thumb.isEmpty()) thumb = QUrl::fromLocalFile(c.thumb).toString() + "?v=" + QString::number(QFileInfo(c.thumb).lastModified().toMSecsSinceEpoch());
    return QVariantMap{{"path", c.raw}, {"name", c.name}, {"date", c.created.toString("yyyy-MM-dd HH:mm:ss")}, {"day", c.created.toString("ddd d MMM yyyy")},
                       {"time", c.created.toString("HH:mm:ss")}, {"duration", c.duration}, {"events", c.events}, {"mb", c.bytes / 1048576.0},
                       {"description", c.description}, {"thumb", thumb}, {"measurements", c.measurements}, {"lastMeasurement", c.lastMeasurement},
                       {"hasStats", c.hasStats}, {"triggers", c.triggers}, {"checked", checked_.contains(c.raw)}, {"selected", c.raw == selected_},
                       {"peakRate", c.peakRate}};
}

void LibraryController::applyView() {
    std::vector<const ClipInfo*> v;
    const QString f = filter_.trimmed();
    for (const ClipInfo& c : all_)
        if (f.isEmpty() || c.name.contains(f, Qt::CaseInsensitive) || c.description.contains(f, Qt::CaseInsensitive)) v.push_back(&c);
    std::stable_sort(v.begin(), v.end(), [&](const ClipInfo* a, const ClipInfo* b) {
        if (sort_ == "name") return a->name.compare(b->name, Qt::CaseInsensitive) < 0;
        if (sort_ == "size") return a->bytes > b->bytes;
        if (sort_ == "events") return a->events > b->events;
        if (sort_ == "measured") return a->measurements > b->measurements;
        return a->created > b->created;
    });
    view_.clear();
    for (const ClipInfo* c : v) view_ << toVariant(*c);
    emit clipsChanged();
}

void LibraryController::setFilter(const QString& text) { filter_ = text; applyView(); }
void LibraryController::setSort(const QString& key) { sort_ = key; QSettings().setValue("evs/lib/sort", key); applyView(); }

void LibraryController::select(const QString& path) {
    selected_ = path;
    for (QVariant& v : view_) { QVariantMap m = v.toMap(); m["selected"] = m["path"] == path; v = m; }
    emit clipsChanged();
    updateDetail();
}

void LibraryController::toggleChecked(const QString& path) {
    if (checked_.contains(path)) checked_.removeAll(path); else checked_ << path;
    for (QVariant& v : view_) { QVariantMap m = v.toMap(); if (m["path"] == path) { m["checked"] = checked_.contains(path); v = m; } }
    emit clipsChanged();
    emit checkedChanged();
}

void LibraryController::checkAll(bool on) {
    checked_.clear();
    if (on) for (const QVariant& v : view_) checked_ << v.toMap()["path"].toString();
    applyView();
    emit checkedChanged();
}

void LibraryController::updateDetail() {
    detail_.clear();
    const auto it = std::find_if(all_.begin(), all_.end(), [&](const ClipInfo& c) { return c.raw == selected_; });
    if (it == all_.end()) { emit detailChanged(); return; }
    const ClipInfo& c = *it;
    detail_ = toVariant(c);
    const QJsonObject& m = c.meta;
    QVariantList rows;
    auto add = [&](const QString& k, const QString& v) { if (!v.isEmpty()) rows << QVariantMap{{"k", k}, {"v", v}}; };
    add("File", QFileInfo(c.raw).fileName());
    add("Recorded", c.created.toString("yyyy-MM-dd HH:mm:ss"));
    add("Size", QString("%1 MB").arg(c.bytes / 1048576.0, 0, 'f', 1));
    if (c.hasStats) {
        add("Duration", QString("%1 s").arg(c.duration, 0, 'f', 3));
        add("Events", QString("%1 M  (%2 % ON)").arg(c.events / 1e6, 0, 'f', 2).arg(100 * c.onFrac, 0, 'f', 0));
        add("Mean rate", c.duration > 0 ? QString("%1 Mev/s").arg(c.events / c.duration / 1e6, 0, 'f', 2) : QString());
        add("Peak rate", QString("%1 Mev/s (1 ms)").arg(c.peakRate / 1e3, 0, 'f', 2));
        add("Ext. triggers", c.triggers ? QString::number(c.triggers) : QString("none"));
        add("Sensor", QString("%1 × %2").arg(c.width).arg(c.height));
    }
    add("Source", m["source"].toString());
    add("Recorder", m["recorder"].toString() + (m.contains("recorder_version") ? QString(" v%1").arg(m["recorder_version"].toInt()) : QString()));
    const QJsonObject b = m["biases"].toObject();
    if (!b.isEmpty())
        add("Biases", QString("on %1 · off %2 · fo %3 · hpf %4 · refr %5").arg(b["bias_diff_on"].toDouble()).arg(b["bias_diff_off"].toDouble())
                          .arg(b["bias_fo"].toDouble()).arg(b["bias_hpf"].toDouble()).arg(b["bias_refr"].toDouble()));
    add("Preset", m["preset"].toString());
    const QJsonObject hf = m["hardware_filters"].toObject();
    if (!hf.isEmpty()) {
        QStringList f;
        if (hf["afk_enable"].toBool()) { const QJsonArray a = hf["afk_band_stop_hz"].toArray(); f << QString("anti-flicker %1–%2 Hz").arg(a[0].toDouble()).arg(a[1].toDouble()); }
        if (hf["erc_enable"].toBool()) f << QString("ERC %1 Mev/s").arg(hf["erc_rate_mev_s"].toDouble());
        if (hf["trail_enable"].toBool()) f << "trail filter";
        if (hf["roi_enable"].toBool()) { const QJsonArray a = hf["roi"].toArray(); f << QString("crop %1,%2 %3×%4").arg(a[0].toDouble()).arg(a[1].toDouble()).arg(a[2].toDouble()).arg(a[3].toDouble()); }
        add("Camera filters", f.isEmpty() ? QString("none") : f.join(", "));
    }
    if (m.contains("trigger_s_in_file")) add("Trigger", QString("%1 s into the file (pre %2 s, length %3 s)").arg(m["trigger_s_in_file"].toDouble(), 0, 'f', 3)
                                                             .arg(m["pre_s"].toDouble(), 0, 'f', 2).arg(m["len_s"].toDouble(), 0, 'f', 2));
    // fields of older recorders are shown as plain metadata
    if (m.contains("label")) add("Label", m["label"].toString());
    if (m.contains("live_rois")) add("Live ROIs", QString::number(m["live_rois"].toArray().size()));
    detail_["rows"] = rows;
    QVariantList ms;
    for (const SavedMeasurement& s : list_measurements(c.raw)) {
        QVariantList res;
        for (const QJsonValue& v : s.results) {
            const QJsonObject r = v.toObject();
            res << QVariantMap{{"name", r["name"].toString()}, {"f", r["f_hz"].toDouble()}, {"snr", r["snr"].toDouble()}, {"ok", r["ok"].toBool()}};
        }
        ms << QVariantMap{{"name", s.name}, {"dir", s.dir}, {"created", s.created.left(16).replace('T', ' ')}, {"results", res},
                          {"window", s.settings.winMode == "onset" ? QString("onset %1 +%2 ms, %3 ms").arg(s.settings.onset + 1).arg(s.settings.skip * 1000, 0, 'f', 0).arg(s.settings.len * 1000, 0, 'f', 0)
                                                                   : QString("%1–%2 s").arg(s.t0, 0, 'f', 3).arg(s.t1, 0, 'f', 3)},
                          {"signal", s.settings.signal}, {"map", QUrl::fromLocalFile(QDir(s.dir).filePath("map.png")).toString()},
                          {"hasMap", QFileInfo::exists(QDir(s.dir).filePath("map.png"))}};
    }
    detail_["measurementList"] = ms;
    emit detailChanged();
}

void LibraryController::setDescription(const QString& path, const QString& text) {
    QString err;
    if (!patch_sidecar(path, QJsonObject{{"description", text}}, &err)) { emit toast(err); return; }
    clipChanged(path);
}

bool LibraryController::rename(const QString& path, const QString& newName) {
    QString nr, err;
    if (!rename_clip(path, newName, &nr, &err)) { emit toast(err); return false; }
    if (selected_ == path) selected_ = nr;
    checked_.replaceInStrings(QRegularExpression("^" + QRegularExpression::escape(path) + "$"), nr);
    loadClips();
    return true;
}

bool LibraryController::moveTo(const QString& path, const QString& project) {
    QString nr, err;
    if (!move_clip(path, lib_.projectDir(project), &nr, &err)) { emit toast(err); return false; }
    checked_.removeAll(path);
    scanProjects();
    loadClips();
    emit checkedChanged();
    emit toast(QFileInfo(path).completeBaseName() + " moved to " + (project.isEmpty() ? QString("Unsorted") : project));
    return true;
}

bool LibraryController::trash(const QString& path) {
    QString err;
    if (!trash_clip(path, lib_.root, &err)) { emit toast(err); return false; }
    checked_.removeAll(path);
    scanProjects();
    loadClips();
    emit checkedChanged();
    emit toast(QFileInfo(path).completeBaseName() + " moved to " + QDir(lib_.root).filePath("_trash"));
    return true;
}

void LibraryController::reveal(const QString& path) const {
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).isDir() ? path : QFileInfo(path).absolutePath()));
}

void LibraryController::regenerate(const QString& path) {
    QFile::remove(stem_of(path) + ".thumb.png");
    patch_sidecar(path, QJsonObject{{"stats", QJsonObject{}}});
    queueStats(path, true);
    clipChanged(path);
}

void LibraryController::clipChanged(const QString& rawPath) {
    const QString dir = QFileInfo(rawPath).absolutePath();
    if (QDir(dir) != QDir(lib_.projectDir(project_))) { scanProjects(); return; }
    bool found = false;
    for (ClipInfo& c : all_)
        if (c.raw == QFileInfo(rawPath).absoluteFilePath()) { c = clip_info(c.raw, project_); found = true; }
    if (!found) {
        all_.push_back(clip_info(rawPath, project_));
        scanProjects();
    }
    const ClipInfo& c = *std::find_if(all_.begin(), all_.end(), [&](const ClipInfo& x) { return x.raw == QFileInfo(rawPath).absoluteFilePath(); });
    if (!c.hasStats || c.thumb.isEmpty()) queueStats(c.raw, true);
    coalesce_.start();
}

// ---- stats worker ---------------------------------------------------------------------------------------------------------------
void LibraryController::queueStats(const QString& raw, bool front) {
    {
        std::lock_guard<std::mutex> lk(smx_);
        if (std::find(squeue_.begin(), squeue_.end(), raw) != squeue_.end()) return;
        if (front) squeue_.push_front(raw); else squeue_.push_back(raw);
        pendingStats_ = (int)squeue_.size();
    }
    scv_.notify_one();
    emit statsChanged();
}

void LibraryController::statsWorker() {
    for (;;) {
        QString raw;
        {
            std::unique_lock<std::mutex> lk(smx_);
            scv_.wait(lk, [&] { return squit_ || !squeue_.empty(); });
            if (squit_) return;
            raw = squeue_.front();
        }
        QString err;
        // a recording that is still being written grows: wait until its size settles
        const qint64 s0 = QFileInfo(raw).size();
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        if (QFileInfo(raw).size() == s0 && QFileInfo::exists(raw)) make_stats(raw, &err, &scancel_);
        {
            std::lock_guard<std::mutex> lk(smx_);
            if (!squeue_.empty() && squeue_.front() == raw) squeue_.pop_front();
            pendingStats_ = (int)squeue_.size();
        }
        QMetaObject::invokeMethod(this, [this, raw] {
            for (ClipInfo& c : all_) if (c.raw == raw) c = clip_info(c.raw, c.project);
            coalesce_.start();
        }, Qt::QueuedConnection);
    }
}

// ---- batch -------------------------------------------------------------------------------------------------------------------------
QVariantList LibraryController::recipes() const {
    QVariantList l;
    QDirIterator it(lib_.projectDir(project_), {"measurement.json"}, QDir::Files, QDirIterator::Subdirectories);
    std::vector<QVariantMap> v;
    while (it.hasNext()) {
        const QString dir = QFileInfo(it.next()).absolutePath();
        if (!QFileInfo(dir).absolutePath().endsWith(".measure")) continue;
        SavedMeasurement m;
        if (!load_measurement(dir, m)) continue;
        QString clip = QFileInfo(QFileInfo(dir).absolutePath()).fileName();
        clip.chop(8);
        v.push_back(QVariantMap{{"name", m.name}, {"clip", clip}, {"dir", dir}, {"rois", (int)m.rois.size()}, {"created", m.created},
                                {"label", QString("%1  (%2, %3 ROIs)").arg(m.name, clip).arg(m.rois.size())}});
    }
    std::sort(v.begin(), v.end(), [](const QVariantMap& a, const QVariantMap& b) { return a["created"].toString() > b["created"].toString(); });
    for (auto& m : v) l << m;
    return l;
}

void LibraryController::runBatch(const QString& recipeDir, const QString& name) {
    if (batch_["running"].toBool()) return;
    if (checked_.isEmpty()) { emit toast("Tick the recordings to run on first"); return; }
    SavedMeasurement recipe;
    if (!load_measurement(recipeDir, recipe)) { emit toast("Choose a saved measurement as the recipe"); return; }
    if (bth_.joinable()) bth_.join();
    const QStringList clips = checked_;
    const QString outName = name.trimmed().isEmpty() ? recipe.name : name.trimmed();
    const QString csv = QDir(lib_.projectDir(project_)).filePath("batch_" + safe_name(outName) + "_" + QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss") + ".csv");
    batch_ = QVariantMap{{"running", true}, {"done", 0}, {"total", clips.size()}, {"name", outName}, {"csv", csv}, {"current", QString()}, {"rows", QVariantList()}};
    emit batchChanged();
    bcancel_ = false;
    bth_ = std::thread([this, clips, recipe, outName, csv] {
        QFile f(csv);
        QTextStream ts(&f);
        const bool ok = f.open(QIODevice::WriteOnly | QIODevice::Text);
        if (ok) ts << "clip,window_t0_s,window_t1_s,roi,f_hz,snr,amp_rel,accepted,decay_s,events,note\n";
        int done = 0;
        for (const QString& raw : clips) {
            if (bcancel_) break;
            QMetaObject::invokeMethod(this, [this, raw] { batch_["current"] = QFileInfo(raw).completeBaseName(); emit batchChanged(); }, Qt::QueuedConnection);
            Clip c;
            QString err;
            QVariantMap row{{"clip", QFileInfo(raw).completeBaseName()}};
            if (!c.load(raw, &err, {}, &bcancel_)) {
                row["note"] = err;
                if (ok) ts << QFileInfo(raw).completeBaseName() << ",,,,,,,,,," << err << "\n";
            } else {
                RoiSet rs;
                rs.fromJson(recipe.rois, c.W, c.H);
                const std::vector<Onset> on = find_onsets(c);
                const MeasureResult res = run_measurement(c, rs.rois, recipe.settings, on);
                std::vector<float> mon, moff;
                if (res.note.isEmpty()) {
                    c.countMaps(res.t0, res.t1, mon, moff);
                    for (size_t i = 0; i < mon.size(); ++i) mon[i] += moff[i];
                    save_measurement(raw, outName, rs.rois, recipe.settings, res, render_log(mon, c.W, c.H), &err);
                }
                int nok = 0;
                QStringList tones;
                for (const RoiResult& r : res.rois) {
                    if (ok)
                        ts << c.name << "," << res.t0 << "," << res.t1 << "," << r.name << "," << (r.tone.f > 0 ? QString::number(r.tone.f, 'f', 3) : QString())
                           << "," << (r.tone.f > 0 ? QString::number(r.tone.snr, 'f', 1) : QString()) << "," << (r.tone.f > 0 ? QString::number(r.tone.amp, 'g', 4) : QString())
                           << "," << (r.ok ? 1 : 0) << "," << (std::isfinite(r.decayS) ? QString::number(r.decayS, 'f', 4) : QString()) << "," << r.events << "," << res.note << "\n";
                    if (r.ok) { ++nok; tones << QString("%1 %2").arg(r.name).arg(r.tone.f, 0, 'f', 1); }
                }
                if (res.rois.empty() && ok) ts << c.name << ",,,,,,,,,," << res.note << "\n";
                row["ok"] = nok;
                row["rois"] = (int)res.rois.size();
                row["tones"] = tones.join(", ");
                row["note"] = res.note;
            }
            ++done;
            QMetaObject::invokeMethod(this, [this, done, row, raw] {
                batch_["done"] = done;
                QVariantList rows = batch_["rows"].toList();
                rows << row;
                batch_["rows"] = rows;
                emit batchChanged();
                clipChanged(raw);
            }, Qt::QueuedConnection);
        }
        f.close();
        QMetaObject::invokeMethod(this, [this, csv] {
            batch_["running"] = false;
            batch_["current"] = QString();
            emit batchChanged();
            emit toast("Batch finished: " + csv);
        }, Qt::QueuedConnection);
    });
}

void LibraryController::cancelBatch() { bcancel_ = true; }

QString LibraryController::urlToPath(const QVariant& url) const { return url.toUrl().toLocalFile(); }
QString LibraryController::pathToUrl(const QString& path) const { return QUrl::fromLocalFile(path).toString(); }

}  // namespace evs

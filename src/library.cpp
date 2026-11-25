#include "library.hpp"
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <algorithm>
#include "measure.hpp"

namespace evs {

QString stem_of(const QString& raw) { return raw.endsWith(".raw", Qt::CaseInsensitive) ? raw.left(raw.size() - 4) : raw; }

static QJsonObject read_json(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

ClipInfo clip_info(const QString& raw, const QString& project) {
    ClipInfo c;
    const QFileInfo fi(raw);
    c.raw = fi.absoluteFilePath();
    c.name = fi.completeBaseName();
    c.project = project;
    c.bytes = fi.size();
    c.meta = read_json(stem_of(c.raw) + ".json");
    c.created = QDateTime::fromString(c.meta["created"].toString(), Qt::ISODate);
    if (!c.created.isValid()) {
        // name_YYYYMMDD_HHMMSS (hertzyevs naming), else the file time
        const QRegularExpressionMatch m = QRegularExpression("(\\d{8}_\\d{6})").match(c.name);
        if (m.hasMatch()) c.created = QDateTime::fromString(m.captured(1), "yyyyMMdd_HHmmss");
        if (!c.created.isValid()) c.created = fi.lastModified();
    }
    c.description = c.meta["description"].toString();
    if (c.description.isEmpty()) c.description = c.meta["label"].toString();
    const QJsonObject st = c.meta["stats"].toObject();
    c.hasStats = st["version"].toInt() >= kStatsVersion;
    c.duration = st["duration_s"].toDouble(c.meta["len_s"].toDouble() + c.meta["pre_s"].toDouble());
    c.events = st["events"].toDouble(c.meta["events"].toDouble());
    c.peakRate = st["peak_rate_ev_ms"].toDouble();
    c.onFrac = st["on_fraction"].toDouble();
    c.triggers = st["ext_triggers"].toInt();
    c.width = st["width"].toInt(1280);
    c.height = st["height"].toInt(720);
    const QString th = stem_of(c.raw) + ".thumb.png";
    if (QFileInfo::exists(th)) c.thumb = th;
    const QDir md(measure_dir(c.raw));
    if (md.exists()) {
        const QFileInfoList ms = md.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Time);
        for (const QFileInfo& m : ms) if (QFileInfo::exists(QDir(m.absoluteFilePath()).filePath("measurement.json"))) {
            if (c.measurements == 0) c.lastMeasurement = m.fileName();
            ++c.measurements;
        }
    }
    return c;
}

bool patch_sidecar(const QString& raw, const QJsonObject& patch, QString* err) {
    const QString path = stem_of(raw) + ".json";
    QJsonObject o = read_json(path);
    if (o.isEmpty()) { o["raw"] = QFileInfo(raw).fileName(); o["recorder"] = "unknown"; }
    for (auto it = patch.begin(); it != patch.end(); ++it) o[it.key()] = it.value();
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) { if (err) *err = "Cannot write " + path; return false; }
    f.write(QJsonDocument(o).toJson());
    return true;
}

bool make_stats(const QString& raw, QString* err, const std::atomic<bool>* cancel) {
    ScanOut s;
    if (!scan_raw(raw, s, err, {}, cancel)) {
        if (cancel && cancel->load()) return false;
        // an empty / broken file still gets stats so it is not scanned again
        patch_sidecar(raw, QJsonObject{{"stats", QJsonObject{{"version", kStatsVersion}, {"events", 0.0}, {"duration_s", 0.0}, {"error", err ? *err : QString()}}}});
        return false;
    }
    float peak = 0;
    for (float r : s.rate) peak = std::max(peak, r);
    int rising = 0;
    for (const TrigEv& t : s.triggers) rising += t.value == 1;
    QJsonObject st{{"version", kStatsVersion}, {"duration_s", s.duration}, {"events", (double)s.events},
                   {"on_fraction", s.events ? double(s.on) / s.events : 0.0}, {"peak_rate_ev_ms", (double)peak},
                   {"mean_rate_ev_s", s.duration > 0 ? s.events / s.duration : 0.0}, {"ext_triggers", rising},
                   {"width", s.W}, {"height", s.H}};
    if (!patch_sidecar(raw, QJsonObject{{"stats", st}}, err)) return false;
    thumbnail(s.count, s.W, s.H, 384).save(stem_of(raw) + ".thumb.png");
    return true;
}

QStringList companions(const QString& raw) {
    const QString st = stem_of(raw);
    QStringList l;
    for (const QString& p : {raw, st + ".json", st + ".thumb.png", st + "_disp.csv", measure_dir(raw)})
        if (QFileInfo::exists(p)) l << p;
    return l;
}

static bool move_set(const QString& raw, const QString& destDir, const QString& newStem, QString* newRaw, QString* err) {
    const QString oldStem = stem_of(raw);
    const QStringList files = companions(raw);
    QStringList targets;
    for (const QString& p : files) {
        const QString suffix = p.mid(oldStem.size());
        targets << QDir(destDir).filePath(newStem + suffix);
    }
    for (const QString& t : targets)
        if (QFileInfo::exists(t)) { if (err) *err = QFileInfo(t).fileName() + " already exists"; return false; }
    QDir().mkpath(destDir);
    for (int i = 0; i < files.size(); ++i) {
        if (!QDir().rename(files[i], targets[i])) {
            for (int j = 0; j < i; ++j) QDir().rename(targets[j], files[j]);   // undo
            if (err) *err = "Cannot move " + QFileInfo(files[i]).fileName() + " (in use?)";
            return false;
        }
    }
    const QString nr = QDir(destDir).filePath(newStem + ".raw");
    if (newRaw) *newRaw = nr;
    if (newStem != QFileInfo(oldStem).fileName()) patch_sidecar(nr, QJsonObject{{"raw", newStem + ".raw"}});
    return true;
}

bool rename_clip(const QString& raw, const QString& newName, QString* newRaw, QString* err) {
    QString n = newName.trimmed();
    if (n.endsWith(".raw", Qt::CaseInsensitive)) n.chop(4);
    n.replace(QRegularExpression("[^A-Za-z0-9_\\-.]"), "_");
    if (n.isEmpty()) { if (err) *err = "Empty name"; return false; }
    if (n == QFileInfo(raw).completeBaseName()) { if (newRaw) *newRaw = raw; return true; }
    return move_set(raw, QFileInfo(raw).absolutePath(), n, newRaw, err);
}

bool move_clip(const QString& raw, const QString& destDir, QString* newRaw, QString* err) {
    if (QDir(destDir).absolutePath() == QFileInfo(raw).absolutePath()) { if (newRaw) *newRaw = raw; return true; }
    return move_set(raw, destDir, QFileInfo(raw).completeBaseName(), newRaw, err);
}

bool trash_clip(const QString& raw, const QString& root, QString* err) {
    const QString rel = QDir(root).relativeFilePath(QFileInfo(raw).absolutePath());
    QString dest = QDir(root).filePath("_trash/" + (rel == "." ? QString() : rel));
    QString stem = QFileInfo(raw).completeBaseName();
    for (int k = 2; QFileInfo::exists(QDir(dest).filePath(stem + ".raw")); ++k) stem = QFileInfo(raw).completeBaseName() + QString("_%1").arg(k);
    return move_set(raw, dest, stem, nullptr, err);
}

// ---- library ------------------------------------------------------------------------------------------------------------------
static bool is_project_dir(const QFileInfo& fi) {
    const QString n = fi.fileName();
    return fi.isDir() && !n.startsWith('.') && !n.startsWith('_') && !n.endsWith(".measure") && n != "camera_xml";
}

QStringList Library::projects() const {
    QStringList l;
    for (const QFileInfo& fi : QDir(root).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::IgnoreCase))
        if (is_project_dir(fi)) l << fi.fileName();
    return l;
}

QString Library::projectDir(const QString& project) const { return project.isEmpty() ? root : QDir(root).filePath(project); }

std::vector<ClipInfo> Library::clips(const QString& project) const {
    std::vector<ClipInfo> v;
    for (const QFileInfo& fi : QDir(projectDir(project)).entryInfoList({"*.raw"}, QDir::Files)) v.push_back(clip_info(fi.absoluteFilePath(), project));
    std::sort(v.begin(), v.end(), [](const ClipInfo& a, const ClipInfo& b) { return a.created > b.created; });
    return v;
}

int Library::clipCount(const QString& project) const { return (int)QDir(projectDir(project)).entryList({"*.raw"}, QDir::Files).size(); }

bool Library::createProject(const QString& name, QString* err) const {
    QString n = name.trimmed();
    n.replace(QRegularExpression("[^A-Za-z0-9_\\- ]"), "_");
    if (n.isEmpty() || n.startsWith('_') || n.startsWith('.')) { if (err) *err = "Use letters, digits, - and _"; return false; }
    const QString d = QDir(root).filePath(n);
    if (QFileInfo::exists(d)) { if (err) *err = n + " already exists"; return false; }
    if (!QDir().mkpath(d)) { if (err) *err = "Cannot create " + d; return false; }
    QFile f(QDir(d).filePath("project.json"));
    if (f.open(QIODevice::WriteOnly))
        f.write(QJsonDocument(QJsonObject{{"name", n}, {"created", QDateTime::currentDateTime().toString(Qt::ISODate)}, {"description", ""}}).toJson());
    return true;
}

QString Library::projectDescription(const QString& project) const {
    return read_json(QDir(projectDir(project)).filePath("project.json"))["description"].toString();
}

bool Library::setProjectDescription(const QString& project, const QString& text) const {
    const QString p = QDir(projectDir(project)).filePath("project.json");
    QJsonObject o = read_json(p);
    o["description"] = text;
    if (!o.contains("name")) o["name"] = project;
    QFile f(p);
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(QJsonDocument(o).toJson());
    return true;
}

}  // namespace evs

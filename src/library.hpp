// Recordings library on disk:
//   <root>/<project>/<clip>.raw          the recording (Metavision EVT3 RAW)
//                    <clip>.json         sidecar: camera settings, description, stats (added by the library)
//                    <clip>.thumb.png    activity-map thumbnail
//                    <clip>.measure/<name>/   saved measurements (measure.hpp)
//   <root>/<project>/project.json        project description
//   <root>/_trash/<project>/...          deleted recordings (moved, not erased)
// Clips directly in <root> belong to the "Unsorted" project (empty name).
#pragma once
#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <vector>
#include "clip.hpp"

namespace evs {

struct ClipInfo {
    QString raw, name, project;
    QDateTime created;
    qint64 bytes = 0;
    QString description;
    QJsonObject meta;
    bool hasStats = false;
    double duration = 0, events = 0, peakRate = 0, onFrac = 0;   // peakRate: events / ms
    int triggers = 0, width = 0, height = 0;
    QString thumb;                       // path, empty when not made yet
    int measurements = 0;
    QString lastMeasurement;             // name of the newest one
};

QString stem_of(const QString& raw);     // path without .raw
ClipInfo clip_info(const QString& raw, const QString& project);
// merge keys into the sidecar (created when missing); false on a write error
bool patch_sidecar(const QString& raw, const QJsonObject& patch, QString* err = nullptr);
// scan the file once: stats into the sidecar + thumbnail PNG
bool make_stats(const QString& raw, QString* err, const std::atomic<bool>* cancel = nullptr);
constexpr int kStatsVersion = 1;

QStringList companions(const QString& raw);   // existing files / folders that belong to the clip (incl. the .raw)
bool rename_clip(const QString& raw, const QString& newName, QString* newRaw, QString* err);
bool move_clip(const QString& raw, const QString& destDir, QString* newRaw, QString* err);
bool trash_clip(const QString& raw, const QString& root, QString* err);

class Library {
public:
    QString root;
    QStringList projects() const;        // sub-folders that are projects, sorted; "" (Unsorted) not included
    QString projectDir(const QString& project) const;
    std::vector<ClipInfo> clips(const QString& project) const;   // newest first
    int clipCount(const QString& project) const;
    bool createProject(const QString& name, QString* err) const;
    QString projectDescription(const QString& project) const;
    bool setProjectDescription(const QString& project, const QString& text) const;
};

}  // namespace evs

// Event sources for the recorder: the Triton2 EVS through the Arena SDK, or the simulator.
// A source delivers chunks from its own acquisition thread through `sink`. Camera features are addressed by a
// logical key (bias_diff_on, afk_enable, ...) that the source maps to its own node names.
#pragma once
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>
#include "evt3.hpp"

namespace evs {

struct Chunk {
    std::vector<uint8_t> raw;     // EVT3 bytes (encoded stream), empty when the source decoded already
    std::vector<Ev> ev;           // decoded events (decoded stream / simulator)
};

// one GenICam node, for the expert table
struct NodeInfo {
    QString name, display, type, value, tip;
    bool writable = false, readable = false;
    double min = 0, max = 0, inc = 0;
    QStringList entries;
};

// logical features, in the order of the camera panel
inline const QStringList kFeatureKeys = {
    "bias_diff_on", "bias_diff_off", "bias_fo", "bias_hpf", "bias_refr",
    "erc_enable", "erc_rate",
    "afk_enable", "afk_mode", "afk_low", "afk_high",
    "trail_enable",
    "roi_enable", "roi_x", "roi_y", "roi_w", "roi_h"};

class EventSource {
public:
    virtual ~EventSource() = default;
    virtual QString kind() const = 0;              // "camera" | "sim"
    virtual QString description() const = 0;       // model + serial, for the status line and the sidecar
    virtual bool open(QString* err) = 0;
    virtual bool start(QString* err) = 0;
    virtual void stop() = 0;
    virtual bool running() const = 0;
    virtual bool encoded() const = 0;              // chunks carry EVT3 bytes (else decoded events)

    // logical features; get returns false when the camera has no node for the key
    virtual bool setFeature(const QString& key, const QVariant& v, QString* err) = 0;
    virtual bool getFeature(const QString& key, QVariant& v) = 0;
    virtual QString nodeFor(const QString& key) const = 0;    // resolved node name ("" = not available)
    virtual void setNodeOverride(const QString& key, const QString& node) { (void)key; (void)node; }

    // expert access to every node
    virtual std::vector<NodeInfo> nodes(const QString& filter) { (void)filter; return {}; }
    virtual bool setNode(const QString& name, const QString& value, QString* err) { (void)name; (void)value; if (err) *err = "not supported"; return false; }
    virtual QString downloadXml(const QString& dir) { (void)dir; return {}; }

    // stream statistics (event rate, link, lost buffers), refreshed by the acquisition thread
    virtual QVariantMap stats() = 0;
    // simulator only
    virtual void tap() {}
    virtual void setSimParam(const QString& key, const QVariant& v) { (void)key; (void)v; }

    std::function<void(Chunk&&)> sink;
    int width = 1280, height = 720;
};

std::unique_ptr<EventSource> make_sim_source();
std::unique_ptr<EventSource> make_arena_source(bool decodedStream);   // nullptr when built without the Arena SDK
bool arena_available();

}  // namespace evs

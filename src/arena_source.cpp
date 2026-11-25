// Triton2 EVS through the Arena SDK (GenICam). Event data: EventFormat = EVT3_0, StreamEvsOutputFormat =
// RawFrameEncoded (EVT3 bytes, recorded as they arrive) or RawFrameDecoded (Arena::EvsRawDecodedEvent).
//
// The node names of the EVS features (biases, anti-flicker, trail filter, ROI) are not in the Arena SDK docs, so they
// are found at connect time by name patterns over the camera's node map (see kPatterns); the camera page shows the
// result and every node can be set by hand. ErcEnable / ErcRateLimit / EventFormat / StreamEvs* are the documented
// names from the Arena EVS examples.
#include <QDir>
#include <QRegularExpression>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <thread>
#include "source.hpp"

#ifdef HZ_HAVE_ARENA
#include "ArenaApi.h"
#endif

namespace evs {

#ifndef HZ_HAVE_ARENA
bool arena_available() { return false; }
std::unique_ptr<EventSource> make_arena_source(bool) { return nullptr; }
#else
bool arena_available() { return true; }

namespace {

// candidate node-name patterns per logical feature, tried in order (case-insensitive, whole name)
const std::vector<std::pair<QString, QStringList>> kPatterns = {
    {"bias_diff_on", {"BiasEventThresholdPositive", "bias_?diff_?on", "bias.*diff.*on.*", ".*diff.*on.*bias.*", ".*threshold.*(pos|on).*"}},
    {"bias_diff_off", {"BiasEventThresholdNegative", "bias_?diff_?off", "bias.*diff.*off.*", ".*diff.*off.*bias.*", ".*threshold.*(neg|off).*"}},
    {"bias_fo", {"BiasLowPassFilterCutoff", "bias_?fo", "bias.*_?fo(_.*)?", "bias.*(lowpass|low_?pass|lpf).*", ".*low_?pass.*cut.*"}},
    {"bias_hpf", {"BiasHighPassFilterCutoff", "bias_?hpf", "bias.*hpf.*", "bias.*(highpass|high_?pass).*", ".*high_?pass.*cut.*"}},
    {"bias_refr", {"BiasRefractoryPeriod", "bias_?refr", "bias.*refr.*", ".*refractory.*"}},
    {"erc_enable", {"ErcEnable", "erc.*enable"}},
    {"erc_rate", {"ErcRateLimit", "erc.*rate.*"}},
    {"afk_enable", {"AntiFlickerEnable", "(afk|anti_?flicker).*enable", "(afk|anti_?flicker)"}},
    {"afk_mode", {"(afk|anti_?flicker).*(mode|filter_?mode|filtering)"}},
    {"afk_low", {"AntiFlickerFrequencyMin", "(afk|anti_?flicker).*(low|min).*", "(afk|anti_?flicker).*band.*low.*"}},
    {"afk_high", {"AntiFlickerFrequencyMax", "(afk|anti_?flicker).*(high|max).*", "(afk|anti_?flicker).*band.*high.*"}},
    {"trail_enable", {"EventBurstFilterEnable", "(trail|stc).*enable", "event_?trail.*", "(trail|stc).*filter.*"}},
    {"roi_enable", {"(evs|event)?_?roi.*enable", "roi_?mode"}},
    {"roi_x", {"(evs|event)?_?roi.*(offset_?x|x_?offset|left|start_?x|x)"}},
    {"roi_y", {"(evs|event)?_?roi.*(offset_?y|y_?offset|top|start_?y|y)"}},
    {"roi_w", {"(evs|event)?_?roi.*(width|w)"}},
    {"roi_h", {"(evs|event)?_?roi.*(height|h)"}}};

QString gs(const GenICam::gcstring& s) { return QString::fromLatin1(s.c_str()); }

QString typeName(GenApi::EInterfaceType t) {
    switch (t) {
    case GenApi::intfIInteger: return "Integer";
    case GenApi::intfIFloat: return "Float";
    case GenApi::intfIBoolean: return "Boolean";
    case GenApi::intfIEnumeration: return "Enum";
    case GenApi::intfICommand: return "Command";
    case GenApi::intfIString: return "String";
    case GenApi::intfICategory: return "Category";
    default: return "Other";
    }
}

class ArenaSource : public EventSource {
public:
#ifdef _WIN32
    explicit ArenaSource(bool decoded) : decoded_(decoded) {}
#else
    explicit ArenaSource(bool) : decoded_(false) {}   // Linux Arena SDK: encoded EVT3 only
#endif
    ~ArenaSource() override {
        stop();
        if (sys_) {
            if (dev_) sys_->DestroyDevice(dev_);
            Arena::CloseSystem(sys_);
        }
    }
    QString kind() const override { return "camera"; }
    QString description() const override { return desc_; }
    bool encoded() const override { return !decoded_; }
    bool running() const override { return running_; }

    bool open(QString* err) override {
        try {
            if (!sys_) sys_ = Arena::OpenSystem();
            sys_->UpdateDevices(1000);
            std::vector<Arena::DeviceInfo> devs = sys_->GetDevices();
            if (devs.empty()) { if (err) *err = "No LUCID camera found (check the network adapter / PoE and IP settings in ArenaView)"; return false; }
            size_t pick = 0;
            for (size_t i = 0; i < devs.size(); ++i)
                if (gs(devs[i].ModelName()).contains("EC") || gs(devs[i].ModelName()).contains("EVS", Qt::CaseInsensitive)) { pick = i; break; }
            desc_ = gs(devs[pick].ModelName()) + "  SN " + gs(devs[pick].SerialNumber()) + "  " + gs(devs[pick].IpAddressStr());
            dev_ = sys_->CreateDevice(devs[pick]);
            GenApi::INodeMap* nm = dev_->GetNodeMap();
            if (!GenApi::IsImplemented(nm->GetNode("EventFormat"))) {
                if (err) *err = "The camera " + desc_ + " has no EventFormat node: not an EVS camera?";
                return false;
            }
            setStr(nm, "AcquisitionMode", "Continuous");
            setStr(nm, "EventFormat", "EVT3_0");
            GenApi::INodeMap* sm = dev_->GetTLStreamNodeMap();
            // never drop buffers on the host: every event of a recording must arrive
            setStr(sm, "StreamBufferHandlingMode", "OldestFirst");
            setBool(sm, "StreamAutoNegotiatePacketSize", true);
            setBool(sm, "StreamPacketResendEnable", true);
            setStr(sm, "StreamEvsOutputFormat", decoded_ ? "RawFrameDecoded" : "RawFrameEncoded");
            try { width = GenApi::CIntegerPtr(nm->GetNode("Width"))->GetValue(); height = GenApi::CIntegerPtr(nm->GetNode("Height"))->GetValue(); } catch (...) {}
            resolve();
            return true;
        } catch (GenICam::GenericException& e) {
            if (err) *err = QString("Arena: ") + e.GetDescription();
            return false;
        } catch (std::exception& e) {
            if (err) *err = QString("Arena: ") + e.what();
            return false;
        }
    }

    bool start(QString* err) override {
        if (!dev_) { if (err) *err = "camera not open"; return false; }
        if (running_) return true;
        try {
            std::lock_guard<std::mutex> lk(node_mx_);
            dev_->StartStream(64);
        } catch (GenICam::GenericException& e) {
            if (err) *err = QString("StartStream: ") + e.GetDescription();
            return false;
        }
        running_ = true;
        th_ = std::thread([this] { loop(); });
        return true;
    }

    void stop() override {
        if (!running_) return;
        running_ = false;
        if (th_.joinable()) th_.join();
        try { std::lock_guard<std::mutex> lk(node_mx_); dev_->StopStream(); } catch (...) {}
    }

    QString nodeFor(const QString& key) const override {
        auto it = map_.find(key);
        return it == map_.end() ? QString() : it->second;
    }
    void setNodeOverride(const QString& key, const QString& node) override {
        if (node.isEmpty()) { overrides_.erase(key); } else overrides_[key] = node;
        resolve();
    }

    bool setFeature(const QString& key, const QVariant& v, QString* err) override {
        const QString n = nodeFor(key);
        if (n.isEmpty()) { if (err) *err = "no camera node for " + key; return false; }
        return writeNode(n, v, err);
    }

    bool getFeature(const QString& key, QVariant& v) override {
        const QString n = nodeFor(key);
        if (n.isEmpty() || !dev_) return false;
        try {
            std::lock_guard<std::mutex> lk(node_mx_);
            GenApi::INode* node = dev_->GetNodeMap()->GetNode(n.toLatin1().constData());
            if (!node || !GenApi::IsReadable(node)) return false;
            switch (node->GetPrincipalInterfaceType()) {
            case GenApi::intfIInteger: v = (double)GenApi::CIntegerPtr(node)->GetValue(); return true;
            case GenApi::intfIFloat: v = GenApi::CFloatPtr(node)->GetValue(); return true;
            case GenApi::intfIBoolean: v = GenApi::CBooleanPtr(node)->GetValue(); return true;
            default: v = gs(GenApi::CValuePtr(node)->ToString()); return true;
            }
        } catch (...) { return false; }
    }

    std::vector<NodeInfo> nodes(const QString& filter) override {
        std::vector<NodeInfo> out;
        if (!dev_) return out;
        std::lock_guard<std::mutex> lk(node_mx_);
        for (GenApi::INodeMap* nm : {dev_->GetNodeMap(), dev_->GetTLStreamNodeMap()}) {
            GenApi::NodeList_t list;
            nm->GetNodes(list);
            for (GenApi::INode* node : list) {
                try {
                    if (!node->IsFeature()) continue;
                    const auto t = node->GetPrincipalInterfaceType();
                    if (t == GenApi::intfICategory || t == GenApi::intfIPort || t == GenApi::intfIRegister || t == GenApi::intfIEnumEntry) continue;
                    if (!GenApi::IsImplemented(node)) continue;
                    NodeInfo ni;
                    ni.name = gs(node->GetName());
                    ni.display = gs(node->GetDisplayName());
                    if (!filter.isEmpty() && !ni.name.contains(filter, Qt::CaseInsensitive) && !ni.display.contains(filter, Qt::CaseInsensitive)) continue;
                    ni.type = typeName(t);
                    ni.tip = gs(node->GetToolTip());
                    ni.readable = GenApi::IsReadable(node);
                    ni.writable = GenApi::IsWritable(node);
                    if (ni.readable && t != GenApi::intfICommand) ni.value = gs(GenApi::CValuePtr(node)->ToString());
                    if (t == GenApi::intfIInteger && ni.readable) { GenApi::CIntegerPtr p(node); ni.min = (double)p->GetMin(); ni.max = (double)p->GetMax(); ni.inc = (double)p->GetInc(); }
                    if (t == GenApi::intfIFloat && ni.readable) { GenApi::CFloatPtr p(node); ni.min = p->GetMin(); ni.max = p->GetMax(); }
                    if (t == GenApi::intfIEnumeration) {
                        GenApi::StringList_t sym;
                        GenApi::CEnumerationPtr(node)->GetSymbolics(sym);
                        for (const auto& s : sym) ni.entries << gs(s);
                    }
                    out.push_back(std::move(ni));
                } catch (...) {}
            }
        }
        return out;
    }

    bool setNode(const QString& name, const QString& value, QString* err) override { return writeNode(name, value, err); }

    QString downloadXml(const QString& dir) override {
        if (!dev_) return {};
        const QString old = QDir::currentPath();
        QDir().mkpath(dir);
        QDir::setCurrent(dir);
        try { std::lock_guard<std::mutex> lk(node_mx_); dev_->DownloadXml(); } catch (...) {}
        QDir::setCurrent(old);
        return dir;
    }

    QVariantMap stats() override {
        std::lock_guard<std::mutex> lk(stat_mx_);
        return stats_;
    }

private:
    static void setStr(GenApi::INodeMap* nm, const char* n, const char* v) {
        try { GenApi::CEnumerationPtr p(nm->GetNode(n)); if (p && GenApi::IsWritable(p)) p->FromString(v); } catch (...) {}
    }
    static void setBool(GenApi::INodeMap* nm, const char* n, bool v) {
        try { GenApi::CBooleanPtr p(nm->GetNode(n)); if (p && GenApi::IsWritable(p)) p->SetValue(v); } catch (...) {}
    }

    bool writeNode(const QString& n, const QVariant& v, QString* err) {
        if (!dev_) { if (err) *err = "camera not open"; return false; }
        try {
            std::lock_guard<std::mutex> lk(node_mx_);
            GenApi::INode* node = dev_->GetNodeMap()->GetNode(n.toLatin1().constData());
            if (!node) node = dev_->GetTLStreamNodeMap()->GetNode(n.toLatin1().constData());
            if (!node) { if (err) *err = "no node " + n; return false; }
            if (!GenApi::IsWritable(node)) { if (err) *err = n + " is not writable now"; return false; }
            switch (node->GetPrincipalInterfaceType()) {
            case GenApi::intfIInteger: {
                GenApi::CIntegerPtr p(node);
                int64_t x = (int64_t)std::llround(v.toDouble());
                const int64_t inc = std::max<int64_t>(1, p->GetInc());
                x = std::clamp(p->GetMin() + (x - p->GetMin()) / inc * inc, p->GetMin(), p->GetMax());
                p->SetValue(x);
                break;
            }
            case GenApi::intfIFloat: {
                GenApi::CFloatPtr p(node);
                p->SetValue(std::clamp(v.toDouble(), p->GetMin(), p->GetMax()));
                break;
            }
            case GenApi::intfIBoolean: {
                const QString s = v.toString().toLower();
                GenApi::CBooleanPtr(node)->SetValue(s == "true" || s == "1" || s == "on");
                break;
            }
            case GenApi::intfICommand: GenApi::CCommandPtr(node)->Execute(); break;
            case GenApi::intfIEnumeration: {
                // match an entry loosely ("BandStop" finds "Band_Stop", "BAND_STOP", "Stop", ...)
                GenApi::CEnumerationPtr p(node);
                GenApi::StringList_t sym;
                p->GetSymbolics(sym);
                auto norm = [](QString s) { return s.toLower().remove(QRegularExpression("[^a-z0-9]")); };
                const QString want = norm(v.toString());
                QString hit;
                for (const auto& s : sym) if (norm(gs(s)) == want) hit = gs(s);
                if (hit.isEmpty()) for (const auto& s : sym) if (norm(gs(s)).contains(want) || want.contains(norm(gs(s)))) { hit = gs(s); break; }
                if (hit.isEmpty()) { if (err) *err = n + " has no entry " + v.toString(); return false; }
                p->FromString(hit.toLatin1().constData());
                break;
            }
            default: GenApi::CValuePtr(node)->FromString(v.toString().toLatin1().constData());
            }
            return true;
        } catch (GenICam::GenericException& e) {
            if (err) *err = n + ": " + e.GetDescription();
            return false;
        }
    }

    void resolve() {
        map_.clear();
        if (!dev_) return;
        std::lock_guard<std::mutex> lk(node_mx_);
        GenApi::NodeList_t list;
        dev_->GetNodeMap()->GetNodes(list);
        QStringList names;
        for (GenApi::INode* n : list) {
            try {
                if (!n->IsFeature() || !GenApi::IsImplemented(n)) continue;
                const auto t = n->GetPrincipalInterfaceType();
                if (t == GenApi::intfICategory || t == GenApi::intfIEnumEntry || t == GenApi::intfIPort) continue;
                names << gs(n->GetName());
            } catch (...) {}
        }
        for (const auto& kp : kPatterns) {
            auto ov = overrides_.find(kp.first);
            if (ov != overrides_.end()) { map_[kp.first] = ov->second; continue; }
            for (const QString& pat : kp.second) {
                const QRegularExpression re("^" + pat + "$", QRegularExpression::CaseInsensitiveOption);
                QString hit;
                for (const QString& n : names) if (re.match(n).hasMatch()) { hit = n; break; }
                // don't hand one node to two features (afk_low vs afk_high, roi_x vs roi_w)
                bool used = false;
                for (const auto& m : map_) if (m.second == hit) used = true;
                if (!hit.isEmpty() && !used) { map_[kp.first] = hit; break; }
            }
        }
    }

    static double num(GenApi::INodeMap* nm, const char* n) {
        try {
            GenApi::INode* node = nm->GetNode(n);
            if (!node || !GenApi::IsReadable(node)) return NAN;
            return gs(GenApi::CValuePtr(node)->ToString()).toDouble();
        } catch (...) { return NAN; }
    }

    void loop() {
        auto last = std::chrono::steady_clock::now();
        uint64_t buffers = 0, bytes = 0;
        while (running_) {
            Arena::IImage* img = nullptr;
            try {
                img = dev_->GetImage(200);
            } catch (GenICam::TimeoutException&) {
                img = nullptr;
            } catch (GenICam::GenericException& e) {
                std::lock_guard<std::mutex> lk(stat_mx_);
                stats_["error"] = QString(e.GetDescription());
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            if (img) {
                const uint8_t* d = img->GetData();
                const size_t n = img->GetSizeFilled();
                ++buffers;
                bytes += n;
                Chunk c;
                if (!decoded_) {
                    c.raw.assign(d, d + n);
                } else {
#ifdef _WIN32   // the Linux Arena SDK has no EVS API (no EvsRawDecodedEvent): there only encoded EVT3 works
                    const size_t k = n / sizeof(Arena::EvsRawDecodedEvent);
                    c.ev.resize(k);
                    const auto* e = reinterpret_cast<const Arena::EvsRawDecodedEvent*>(d);
                    for (size_t i = 0; i < k; ++i) c.ev[i] = Ev{e[i].x, e[i].y, uint8_t(e[i].p > 0 ? 1 : 0), (int64_t)e[i].ts};
#endif
                }
                try { dev_->RequeueBuffer(img); } catch (...) {}
                if (sink && (!c.raw.empty() || !c.ev.empty())) sink(std::move(c));
            }
            const auto now = std::chrono::steady_clock::now();
            const double dt = std::chrono::duration<double>(now - last).count();
            if (dt > 0.5) {
                QVariantMap s;
                {
                    std::lock_guard<std::mutex> lk(node_mx_);
                    GenApi::INodeMap* sm = dev_->GetTLStreamNodeMap();
                    s["cameraRate"] = num(sm, "StreamEvsEventRate");
                    s["linkBps"] = num(sm, "StreamEvsLinkThroughput");
                    s["gvspRate"] = num(sm, "StreamEvsGvspFrameRate");
                    s["lost"] = num(sm, "StreamLostFrameCount");
                    s["missedPackets"] = num(sm, "StreamMissedPacketCount");
                    s["missedImages"] = num(sm, "StreamMissedImageCount");
                    s["incomplete"] = num(sm, "StreamIncompleteFrameCount");
                    GenApi::INodeMap* nm = dev_->GetNodeMap();
                    s["temperature"] = num(nm, "DeviceTemperature");
                }
                s["buffersPerS"] = buffers / dt;
                s["hostBps"] = bytes / dt;
                std::lock_guard<std::mutex> lk(stat_mx_);
                stats_ = s;
                buffers = bytes = 0;
                last = now;
            }
        }
    }

    bool decoded_ = false;
    Arena::ISystem* sys_ = nullptr;
    Arena::IDevice* dev_ = nullptr;
    QString desc_;
    std::map<QString, QString> map_, overrides_;
    std::thread th_;
    std::atomic<bool> running_{false};
    std::mutex node_mx_, stat_mx_;
    QVariantMap stats_;
};

}  // namespace

std::unique_ptr<EventSource> make_arena_source(bool decodedStream) { return std::make_unique<ArenaSource>(decodedStream); }
#endif

}  // namespace evs

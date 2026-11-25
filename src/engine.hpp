// Event processing for the recorder, on its own thread so the UI never waits for the camera:
//   decode EVT3 -> live frame (60 fps), ROI signals (10 kHz bins), event rate (1 ms bins), record trigger,
//   repeating measure trigger (event-rate jump or external trigger input), pre-trigger buffer and the
//   recording writer (raw camera bytes into a Metavision .raw file).
//
// ROIs come as a pixel look-up table (roi.hpp build_lut): per pixel the ROI index + 1 and a sign. Per ROI and bin the
// engine sums sign over ON events (on), sign over OFF events (off) and counts all events (n). For a sided line ROI the
// sign is -1 / +1 on the two sides, so on and off are proportional to the lateral velocity of an edge.
#pragma once
#include <QImage>
#include <QRectF>
#include <QString>
#include <QVariantMap>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
#include "evt3.hpp"
#include "source.hpp"

namespace evs {

struct Probe {
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;   // sensor pixels, along the line
    double half = 6;                         // half-width of each side band (px)
    double length() const { return std::hypot(x1 - x0, y1 - y0); }
};

// pixel -> (probe, side) map of line probes; 0 = none, else 2*probe + side + 1 (side 0 = +normal, 1 = -normal)
// (used by the offline analyse_raw of analysis.hpp)
std::vector<uint16_t> build_probe_lut(int w, int h, const std::vector<Probe>& probes);

// signals of one ROI at fs: on = sum of the pixel signs over ON events, off = same over OFF events, n = all events
struct ProbeTrace { std::vector<float> on, off, n; };

constexpr double kProbeFs = 10000.0;       // probe bin rate (100 us bins)
constexpr int kProbeBinUs = 100;
constexpr double kRingS = 30.0;           // history kept for the review of the last recording

struct RecStatus {
    int state = 0;                 // 0 idle, 1 armed (waiting for the tap), 2 recording, 3 writing
    QString path;
    int64_t tTrigger = -1, tStart = -1, tEnd = -1;
    int64_t tFirst = -1;           // time of the first event written (start of the file)
    double seconds = 0;            // recorded so far
    uint64_t bytes = 0, events = 0;
    bool finished = false;         // one-shot flag: a recording was completed since the last poll
    QString error;
};

class Engine {
public:
    Engine(int w, int h);
    ~Engine();
    int width() const { return w_; }
    int height() const { return h_; }

    void push(Chunk&& c);                       // acquisition thread
    void setEncoded(bool enc) { encoded_ = enc; }
    void reset();                               // new stream: decoder state, rings
    void setProbes(const std::vector<Probe>& p);   // line probes (tests)
    // ROIs: lut = ROI index + 1 per pixel, sign per pixel, n = number of ROIs
    void setRois(std::vector<uint16_t> lut, std::vector<int8_t> sign, int n);
    void setSoftRoi(bool on, const QRectF& r);
    void setView(int mode, double decayMs, double fps) { view_mode_ = mode; decay_ms_ = decayMs; fps_ = fps; }

    // latest rendered frame (shared, implicitly copied QImage); returns false when nothing new since `seq`
    bool frame(QImage& out, uint64_t& seq);

    // last `seconds` of a probe (fs = kProbeFs), ending at the newest complete bin; tEnd = time of that bin end (us)
    bool probeWindow(int i, double seconds, ProbeTrace& out, int64_t* tEnd);
    bool probeRange(int i, int64_t t0, int64_t t1, ProbeTrace& out);
    // event rate (ev/s) in 1 ms bins: all events and the events inside the soft ROI / probes
    void rateWindow(double seconds, std::vector<float>& all, std::vector<float>& roi);
    QVariantMap stats();                        // rates, queue, decoder health
    int64_t binTime();                          // end of the newest complete ROI bin (us)

    // repeating measure trigger: source 0 = event-rate jump (trigger region), 1 = external trigger input (rising edge)
    void setMeasureTrigger(bool on, int source, double factor, double gapS, bool regionOnly = false);
    void takeMeasureTriggers(std::vector<int64_t>& out);   // trigger times (us) since the last call

    // recording: armed waits for the tap (rate jump in the ROI), else starts now; pre-trigger from the ring
    bool record(const QString& path, const std::string& header, double preS, double lenS, bool armed, double factor);
    void cancel();
    RecStatus recStatus();                      // clears the finished flag

private:
    void loop();
    void process(Chunk& c);
    void handleEvents(const Ev* e, size_t n);
    void advanceBins(int64_t b);
    void advanceRate(int64_t rb);
    void checkTrigger(int64_t done);
    void checkMeasureTrigger(int64_t done);
    void pushMeasureTrigger(int64_t t);
    void render();
    void trigger(int64_t t);
    void startWriting(int64_t tFrom);
    void writerLoop();
    void finishRecording();

    int w_, h_;
    std::atomic<bool> encoded_{true}, quit_{false};
    bool reset_req_ = false;          // qmx_
    std::thread th_, wth_;
    std::mutex qmx_;
    std::condition_variable qcv_;
    std::deque<Chunk> q_;
    size_t q_bytes_ = 0;
    uint64_t dropped_chunks_ = 0;

    Evt3Decoder dec_;
    Evt3Encoder enc_;
    std::vector<Ev> evbuf_;

    // live view state
    std::vector<int64_t> last_t_;
    std::vector<uint8_t> last_p_;
    std::vector<float> act_;
    std::atomic<int> view_mode_{0};
    std::atomic<double> decay_ms_{8.0}, fps_{60.0};
    int64_t t_now_ = 0;
    std::mutex fmx_;
    QImage frame_;
    uint64_t frame_seq_ = 0;
    std::chrono::steady_clock::time_point last_render_{};
    double act_decay_t_ = 0;

    // probes and rates
    std::mutex dmx_;                  // probe rings, lut, rates
    int nroi_ = 0;
    std::vector<uint16_t> lut_;
    std::vector<int8_t> sign_;
    std::vector<uint8_t> roi_mask_;   // soft ROI (1 = inside)
    bool roi_on_ = false;
    QRectF roi_;
    int nbins_ = int(kRingS * kProbeFs);
    std::vector<std::vector<float>> r_on_, r_off_, r_n_;
    int64_t bin_hi_ = -1;             // highest bin index touched (bins < bin_hi_ are complete)
    int nrate_ = int(kRingS * 1000);
    std::vector<float> rate_all_, rate_roi_;
    int64_t rbin_hi_ = -1;
    uint64_t ev_total_ = 0, ev_since_ = 0;
    double ev_rate_ = 0;
    std::chrono::steady_clock::time_point rate_clock_{};

    // measure trigger
    std::mutex mmx_;
    bool mt_on_ = false;
    int mt_src_ = 0;
    bool mt_region_ = false;   // rate of the trigger region (crop / ROIs) instead of the whole sensor
    double mt_factor_ = 4.0, mt_gap_ = 0.15;
    int64_t mt_last_ = INT64_MIN / 4, mt_arm_ = -1;
    std::vector<int64_t> mt_q_;
    std::vector<TrigEv> trig_buf_;
    float act_max_ = 1.f;             // activity view: max of the previous frame (normalisation)

    // pre-trigger ring of raw chunks
    struct RawChunk { std::vector<uint8_t> raw; int64_t t0, t1; uint64_t nev; };
    std::deque<RawChunk> pre_;
    double pre_keep_s_ = 1.0;

    // recording
    std::mutex rmx_;
    RecStatus rec_;
    double rec_pre_ = 0.2, rec_len_ = 2.0, trig_factor_ = 4.0;
    int64_t pending_trig_ = -1;       // trigger time found by the rate check (us)
    bool start_now_ = false;          // manual start: trigger at the next chunk
    int64_t arm_bin_ = 0;
    std::string header_;
    // writer thread
    std::mutex wmx_;
    std::condition_variable wcv_;
    std::deque<std::vector<uint8_t>> wq_;
    bool w_open_ = false, w_close_ = false;
    FILE* wf_ = nullptr;
};

}  // namespace evs

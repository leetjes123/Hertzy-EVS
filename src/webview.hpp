// Read-only live view in a web browser (same network): a tiny HTTP server on its own thread.
//   GET /            page: live picture with the ROIs drawn on top, ROI tones, live spectra, status
//   GET /stream.mjpg MJPEG stream of the live picture (?w=width, default 960; ?fps=, default 15)
//   GET /frame.jpg   one picture
//   GET /live.json   status, ROIs, tones and spectra (set by the controller a few times a second while someone watches)
// No camera control from the browser. JPEGs are encoded on the server thread, once per picture and width for all viewers;
// a viewer that can't keep up skips pictures instead of building a backlog.
#pragma once
#include <QByteArray>
#include <QHash>
#include <QImage>
#include <QObject>
#include <QString>
#include <QStringList>
#include <atomic>
#include <mutex>
#include <vector>

class QTcpServer;
class QTcpSocket;
class QTimer;
class QThread;

namespace evs {

class Engine;

class WebView : public QObject {
    Q_OBJECT
public:
    explicit WebView(Engine* engine);
    ~WebView() override;
    // start / stop the server thread; returns false (and err) when the port can't be opened
    bool start(int port, QString* err);
    void stop();
    bool running() const { return port_ > 0; }
    int port() const { return port_; }
    int viewers() const { return viewers_.load(); }
    QStringList urls() const;                 // http://<ip>:<port>/ for every IPv4 address of this machine
    void setJson(const QByteArray& json);     // any thread
    bool wanted() const;                      // someone is watching: worth building the JSON

private:
    struct Client { QTcpSocket* s = nullptr; QByteArray req; bool stream = false; int width = 960; int fps = 15; qint64 lastMs = 0; uint64_t sentSeq = 0; };
    void onConnection();
    void onReadable(QTcpSocket* s);
    void handle(Client& c);
    void pump();                              // stream timer: push new pictures to the stream viewers
    QByteArray jpeg(int width, uint64_t* seq);   // newest picture, cached per width; seq = its number

    Engine* engine_;
    QThread* th_ = nullptr;
    QTcpServer* srv_ = nullptr;               // lives in th_
    QTimer* timer_ = nullptr;                 // lives in th_
    std::vector<Client> clients_;             // th_ only
    int port_ = 0;
    std::atomic<int> viewers_{0};
    std::atomic<qint64> last_json_ms_{0};
    std::mutex jmx_;
    QByteArray json_ = "{}";
    // picture cache (th_ only)
    uint64_t seq_ = 0, engine_seq_ = 0;
    QImage cur_;
    QHash<int, QByteArray> jpg_;
    QHash<int, uint64_t> jpg_seq_;
};

}  // namespace evs

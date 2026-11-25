#include "webview.hpp"
#include <QBuffer>
#include <QDateTime>
#include <QHostAddress>
#include <QImage>
#include <QNetworkInterface>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <QUrlQuery>
#include <algorithm>
#include "engine.hpp"
#include "prof.hpp"

namespace evs {

// the page: picture + ROI outlines (SVG in sensor pixels), ROI table, spectra (canvas), status line; polls /live.json
static const char* kPage = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>EVS live</title>
<style>
:root{--bg:#111317;--card:#1a1d22;--line:#2a2f37;--text:#e6e8eb;--dim:#9ba1ab;--accent:#4c9bff}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:14px system-ui,sans-serif}
header{display:flex;gap:16px;align-items:center;padding:10px 16px;border-bottom:1px solid var(--line);flex-wrap:wrap}
header b{font-size:16px}#st{color:var(--dim);font-family:ui-monospace,monospace;font-size:13px}
#rec{padding:2px 10px;border-radius:10px;background:var(--card)}
main{display:grid;grid-template-columns:minmax(0,3fr) minmax(260px,1fr);gap:12px;padding:12px}
@media(max-width:900px){main{grid-template-columns:1fr}}
.card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:10px}
.view{position:relative;line-height:0}.view img{width:100%;height:auto;border-radius:6px;background:#000}
.view svg{position:absolute;inset:0;width:100%;height:100%;pointer-events:none}
h3{margin:0 0 8px;font-size:14px}table{width:100%;border-collapse:collapse;font-variant-numeric:tabular-nums}
td,th{padding:4px 6px;text-align:left;border-bottom:1px solid var(--line)}th{color:var(--dim);font-weight:500}
.sw{display:inline-block;width:10px;height:10px;border-radius:2px;margin-right:6px}
canvas{width:100%;height:240px;display:block}.dim{color:var(--dim)}label{color:var(--dim);font-size:13px}
select{background:var(--card);color:var(--text);border:1px solid var(--line);border-radius:6px;padding:2px 4px}
</style></head><body>
<header><b>EVS live</b><span id="src" class="dim"></span><span id="rec"></span><span id="st"></span>
<span style="flex:1"></span><label>size <select id="w"><option>640</option><option selected>960</option><option>1280</option></select></label>
<label>fps <select id="fps"><option>5</option><option>10</option><option selected>15</option><option>25</option></select></label></header>
<main><div class="card"><div class="view"><img id="img" alt="live view"><svg id="ov" viewBox="0 0 1280 720" preserveAspectRatio="none"></svg></div></div>
<div><div class="card"><h3>ROIs</h3><table><thead><tr><th>ROI</th><th>tone</th><th>SNR</th></tr></thead><tbody id="rois"></tbody></table></div>
<div class="card" style="margin-top:12px"><h3>Spectrum <span id="rng" class="dim"></span></h3><canvas id="sp"></canvas></div></div></main>
<script>
const $=id=>document.getElementById(id);
function stream(){ $('img').src='stream.mjpg?w='+$('w').value+'&fps='+$('fps').value+'&t='+Date.now(); }
$('w').onchange=stream; $('fps').onchange=stream; $('img').onerror=()=>setTimeout(stream,1500); stream();
function esc(s){return String(s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]))}
function roiSvg(r){
  const c=esc(r.color), sw=r.selected?3:1.6, p=r.pts||[];
  if(r.type==='line'&&p.length>=2){const dx=p[1].x-p[0].x,dy=p[1].y-p[0].y,L=Math.hypot(dx,dy)||1,nx=-dy/L*r.half,ny=dx/L*r.half;
    return `<polygon points="${p[0].x+nx},${p[0].y+ny} ${p[1].x+nx},${p[1].y+ny} ${p[1].x-nx},${p[1].y-ny} ${p[0].x-nx},${p[0].y-ny}" fill="none" stroke="${c}" stroke-width="${sw}" vector-effect="non-scaling-stroke"/>`;}
  if(r.type==='rect'&&p.length>=2) return `<rect x="${Math.min(p[0].x,p[1].x)}" y="${Math.min(p[0].y,p[1].y)}" width="${Math.abs(p[1].x-p[0].x)}" height="${Math.abs(p[1].y-p[0].y)}" fill="none" stroke="${c}" stroke-width="${sw}" vector-effect="non-scaling-stroke"/>`;
  if(r.type==='polygon'&&p.length>=3) return `<polygon points="${p.map(q=>q.x+','+q.y).join(' ')}" fill="none" stroke="${c}" stroke-width="${sw}" vector-effect="non-scaling-stroke"/>`;
  if(r.type==='mask'&&r.bw>0) return `<rect x="${r.bx}" y="${r.by}" width="${r.bw}" height="${r.bh}" fill="none" stroke="${c}" stroke-dasharray="4 4" stroke-width="1" vector-effect="non-scaling-stroke"/>`;
  return '';}
function label(r){const p=r.pts&&r.pts[0]||{x:r.bx||0,y:r.by||0};return `<text x="${p.x}" y="${p.y-6}" fill="${esc(r.color)}" font-size="16" font-weight="700">${esc(r.name)}${r.ok?' '+r.f.toFixed(1)+' Hz':''}</text>`;}
function plot(d){
  const cv=$('sp'),dpr=devicePixelRatio||1,W=cv.clientWidth,H=cv.clientHeight; cv.width=W*dpr; cv.height=H*dpr;
  const g=cv.getContext('2d'); g.scale(dpr,dpr); g.clearRect(0,0,W,H);
  const L=40,B=20,x0=d.fmin,x1=d.fmax; let ym=0; for(const s of d.spectra) for(const a of s.amp) ym=Math.max(ym,a); ym=ym*1.1||1;
  const X=f=>L+(f-x0)/(x1-x0)*(W-L-6), Y=a=>H-B-a/ym*(H-B-6);
  g.strokeStyle='#2a2f37'; g.fillStyle='#9ba1ab'; g.font='11px system-ui';
  for(let k=0;k<=4;k++){const f=x0+(x1-x0)*k/4,x=X(f); g.beginPath(); g.moveTo(x,6); g.lineTo(x,H-B); g.stroke(); g.fillText(f.toFixed(0),x-10,H-5);}
  g.fillText('Hz',W-20,H-5);
  for(const s of d.spectra){g.strokeStyle=s.color; g.lineWidth=1.3; g.beginPath(); s.f.forEach((f,i)=>{const x=X(f),y=Y(s.amp[i]); i?g.lineTo(x,y):g.moveTo(x,y)}); g.stroke();}
  $('rng').textContent=x0.toFixed(0)+'–'+x1.toFixed(0)+' Hz';}
async function poll(){
  try{const d=await (await fetch('live.json',{cache:'no-store'})).json();
    $('src').textContent=d.source||''; $('rec').textContent=d.recText||''; $('rec').style.color=d.recColor||'';
    $('st').textContent=(d.rate/1e6).toFixed(2)+' Mev/s'+(d.running?'':'  · stream stopped');
    $('ov').setAttribute('viewBox',`0 0 ${d.w} ${d.h}`);
    $('ov').innerHTML=(d.rois||[]).map(r=>roiSvg(r)+label(r)).join('');
    $('rois').innerHTML=(d.rois||[]).map(r=>`<tr><td><span class="sw" style="background:${esc(r.color)}"></span>${esc(r.name)}</td><td>${r.f>0?r.f.toFixed(1)+' Hz':'–'}</td><td>${r.f>0?r.snr.toFixed(0):''}${r.ok?' ✓':''}</td></tr>`).join('')||'<tr><td colspan=3 class="dim">no ROIs</td></tr>';
    plot(d);
  }catch(e){$('st').textContent='connection lost…';}
  setTimeout(poll,300);}
poll();
</script></body></html>)HTML";

WebView::WebView(Engine* engine) : engine_(engine) {}

WebView::~WebView() { stop(); }

bool WebView::start(int port, QString* err) {
    stop();
    th_ = new QThread;
    th_->setObjectName("webview");
    th_->start();
    bool ok = false;
    QString e;
    // everything network-side is created in, and only touched from, the server thread
    auto* ctx = new QObject;
    ctx->moveToThread(th_);
    QMetaObject::invokeMethod(ctx, [&] {
        srv_ = new QTcpServer;
        if (!srv_->listen(QHostAddress::AnyIPv4, (quint16)port)) {
            e = srv_->errorString();
            delete srv_;
            srv_ = nullptr;
            return;
        }
        QObject::connect(srv_, &QTcpServer::newConnection, srv_, [this] { onConnection(); });
        timer_ = new QTimer;
        timer_->setTimerType(Qt::PreciseTimer);
        QObject::connect(timer_, &QTimer::timeout, timer_, [this] { pump(); });
        timer_->start(10);
        ok = true;
    }, Qt::BlockingQueuedConnection);
    ctx->deleteLater();   // only the target of the queued call
    if (!ok) {
        th_->quit();
        th_->wait();
        delete th_;
        th_ = nullptr;
        if (err) *err = e;
        return false;
    }
    port_ = port;
    return true;
}

void WebView::stop() {
    if (!th_) return;
    auto* ctx = new QObject;
    ctx->moveToThread(th_);
    QMetaObject::invokeMethod(ctx, [this] {
        for (Client& c : clients_) { c.s->abort(); delete c.s; }
        clients_.clear();
        delete timer_;
        timer_ = nullptr;
        delete srv_;
        srv_ = nullptr;
        jpg_.clear();
        jpg_seq_.clear();
    }, Qt::BlockingQueuedConnection);
    ctx->deleteLater();
    th_->quit();
    th_->wait();
    delete th_;
    th_ = nullptr;
    port_ = 0;
    viewers_ = 0;
}

QStringList WebView::urls() const {
    QStringList l;
    if (!port_) return l;
    for (const QHostAddress& a : QNetworkInterface::allAddresses())
        if (a.protocol() == QAbstractSocket::IPv4Protocol && !a.isLoopback() && !a.isLinkLocal())
            l << QString("http://%1:%2/").arg(a.toString()).arg(port_);
    if (l.isEmpty()) l << QString("http://localhost:%1/").arg(port_);
    return l;
}

void WebView::setJson(const QByteArray& json) {
    std::lock_guard<std::mutex> lk(jmx_);
    json_ = json;
}

bool WebView::wanted() const {
    return viewers_.load() > 0 || QDateTime::currentMSecsSinceEpoch() - last_json_ms_.load() < 3000;
}

// ---- server thread ----------------------------------------------------------------------------------------------------------
void WebView::onConnection() {
    while (QTcpSocket* s = srv_->nextPendingConnection()) {
        if (clients_.size() >= 32) { s->abort(); s->deleteLater(); continue; }
        Client c;
        c.s = s;
        clients_.push_back(c);
        QObject::connect(s, &QTcpSocket::readyRead, s, [this, s] { onReadable(s); });
        QObject::connect(s, &QTcpSocket::disconnected, s, [this, s] {
            clients_.erase(std::remove_if(clients_.begin(), clients_.end(), [s](const Client& c) { return c.s == s; }), clients_.end());
            viewers_ = (int)std::count_if(clients_.begin(), clients_.end(), [](const Client& c) { return c.stream; });
            s->deleteLater();
        });
    }
}

void WebView::onReadable(QTcpSocket* s) {
    auto it = std::find_if(clients_.begin(), clients_.end(), [s](const Client& c) { return c.s == s; });
    if (it == clients_.end()) return;
    it->req += s->readAll();
    if (it->stream) { it->req.clear(); return; }
    if (it->req.size() > 16384) { s->abort(); return; }
    if (!it->req.contains("\r\n\r\n")) return;
    handle(*it);
}

static QByteArray reply(const QByteArray& status, const QByteArray& type, const QByteArray& body) {
    return "HTTP/1.1 " + status + "\r\nContent-Type: " + type + "\r\nContent-Length: " + QByteArray::number(body.size()) +
           "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n" + body;
}

void WebView::handle(Client& c) {
    const QList<QByteArray> line = c.req.left(c.req.indexOf("\r\n")).split(' ');
    c.req.clear();
    QTcpSocket* s = c.s;
    if (line.size() < 2 || line[0] != "GET") {
        s->write(reply("405 Method Not Allowed", "text/plain", "read-only\n"));
        s->disconnectFromHost();
        return;
    }
    const QString target = QString::fromUtf8(line[1]);
    const int q = target.indexOf('?');
    const QString path = q < 0 ? target : target.left(q);
    const QUrlQuery query(q < 0 ? QString() : target.mid(q + 1));
    const int width = std::clamp(query.hasQueryItem("w") ? query.queryItemValue("w").toInt() : 960, 160, 1280);
    if (path == "/" || path == "/index.html") {
        s->write(reply("200 OK", "text/html; charset=utf-8", QByteArray(kPage)));
    } else if (path == "/live.json") {
        QByteArray j;
        {
            std::lock_guard<std::mutex> lk(jmx_);
            j = json_;
        }
        last_json_ms_ = QDateTime::currentMSecsSinceEpoch();
        s->write(reply("200 OK", "application/json", j));
    } else if (path == "/frame.jpg") {
        const QByteArray img = jpeg(width, nullptr);
        if (img.isEmpty()) s->write(reply("503 Service Unavailable", "text/plain", "no picture yet\n"));
        else s->write(reply("200 OK", "image/jpeg", img));
    } else if (path == "/stream.mjpg") {
        c.stream = true;
        c.width = width;
        c.fps = std::clamp(query.hasQueryItem("fps") ? query.queryItemValue("fps").toInt() : 15, 1, 30);
        c.sentSeq = 0;
        s->write("HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=frame\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n");
        viewers_ = (int)std::count_if(clients_.begin(), clients_.end(), [](const Client& k) { return k.stream; });
        return;   // stays open
    } else {
        s->write(reply("404 Not Found", "text/plain", "not found\n"));
    }
    s->disconnectFromHost();
}

QByteArray WebView::jpeg(int width, uint64_t* seqOut) {
    QImage img;
    uint64_t es = engine_seq_;
    if (engine_ && engine_->frame(img, es)) { cur_ = img; engine_seq_ = es; ++seq_; }
    if (cur_.isNull()) return {};
    if (jpg_seq_.value(width) != seq_) {
        hz::ProfScope prof_("web.jpeg");
        const QImage sc = width >= cur_.width() ? cur_ : cur_.scaledToWidth(width, Qt::SmoothTransformation);
        QByteArray out;
        QBuffer b(&out);
        b.open(QIODevice::WriteOnly);
        sc.save(&b, "JPG", 80);
        jpg_[width] = out;
        jpg_seq_[width] = seq_;
    }
    if (seqOut) *seqOut = seq_;
    return jpg_.value(width);
}

void WebView::pump() {
    if (viewers_.load() == 0) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (Client& c : clients_) {
        if (!c.stream || c.s->state() != QAbstractSocket::ConnectedState) continue;
        if (now - c.lastMs < 1000 / c.fps) continue;
        if (c.s->bytesToWrite() > (2 << 20)) continue;   // slow viewer: skip pictures
        uint64_t sq = 0;
        const QByteArray img = jpeg(c.width, &sq);
        if (img.isEmpty() || sq == c.sentSeq) continue;
        c.s->write("--frame\r\nContent-Type: image/jpeg\r\nContent-Length: " + QByteArray::number(img.size()) + "\r\n\r\n");
        c.s->write(img);
        c.s->write("\r\n");
        c.sentSeq = sq;
        c.lastMs = now;
    }
}

}  // namespace evs

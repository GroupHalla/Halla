#include "WebRtcScreenShareBridge.h"
#include "net/NetSession.h"
#include "core/AppLog.h"

#include <QCloseEvent>
#include <QJsonDocument>
#include <QJsonObject>
#include <QVBoxLayout>
#include <QWebChannel>
#include <QWebEnginePage>
#include <QWebEngineView>

WebRtcScreenShareBridge::WebRtcScreenShareBridge(NetSession* net, QWidget* parent)
    : QDialog(parent), m_net(net) {
    setWindowTitle(tr("Transmissão WebRTC"));
    resize(520, 420);
    QVBoxLayout* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);

    m_view = new QWebEngineView(this);
    root->addWidget(m_view, 1);

    m_channel = new QWebChannel(this);
    m_channel->registerObject(QStringLiteral("bridge"), this);
    m_view->page()->setWebChannel(m_channel);

    connect(m_view->page(), &QWebEnginePage::featurePermissionRequested,
            this, [this](const QUrl& origin, QWebEnginePage::Feature feature) {
        m_view->page()->setFeaturePermission(origin, feature,
                                             QWebEnginePage::PermissionGrantedByUser);
    });

    initPage();
}

WebRtcScreenShareBridge::~WebRtcScreenShareBridge() {
    stopAll();
}

void WebRtcScreenShareBridge::initPage() {
    m_view->setHtml(html(), QUrl(QStringLiteral("https://halla.local/webrtc")));
}

void WebRtcScreenShareBridge::sendOffer(int toUserId, const QString& sdp) {
    if (m_net) m_net->sendWebRtcOffer(toUserId, sdp);
}

void WebRtcScreenShareBridge::sendIce(int toUserId, const QString& candidate,
                                      const QString& sdpMid, int sdpMLineIndex) {
    if (m_net) m_net->sendWebRtcIce(toUserId, candidate, sdpMid, sdpMLineIndex);
}

void WebRtcScreenShareBridge::captureStarted() {
    AppLog::info(tr("Transmissão WebRTC local iniciada"));
}

void WebRtcScreenShareBridge::captureStopped() {
    AppLog::info(tr("Transmissão WebRTC local parada"));
    if (m_net) m_net->sendWebRtcStreamStop();
}

void WebRtcScreenShareBridge::jsLog(const QString& text) {
    AppLog::info(QStringLiteral("WebRTC: %1").arg(text));
}

void WebRtcScreenShareBridge::handleSignal(const QJsonObject& signal) {
    const QString t = signal.value(QStringLiteral("t")).toString();
    const int from = signal.value(QStringLiteral("from")).toInt();
    if (from <= 0) return;
    if (t == QLatin1String("webrtc_watch_request")) {
        runJs(QStringLiteral("window.hallaCreateOffer(%1);").arg(from));
    } else if (t == QLatin1String("webrtc_watch_stop")) {
        runJs(QStringLiteral("window.hallaClosePeer(%1);").arg(from));
    } else if (t == QLatin1String("webrtc_answer")) {
        const QString json = QString::fromUtf8(QJsonDocument(signal).toJson(QJsonDocument::Compact));
        runJs(QStringLiteral("window.hallaHandleAnswer(%1);").arg(json));
    } else if (t == QLatin1String("webrtc_ice")) {
        const QString json = QString::fromUtf8(QJsonDocument(signal).toJson(QJsonDocument::Compact));
        runJs(QStringLiteral("window.hallaHandleIce(%1);").arg(json));
    }
}

void WebRtcScreenShareBridge::stopAll() {
    if (m_view) runJs(QStringLiteral("window.hallaStopAll && window.hallaStopAll();"));
}

void WebRtcScreenShareBridge::runJs(const QString& code) {
    if (m_view && m_view->page()) m_view->page()->runJavaScript(code);
}

void WebRtcScreenShareBridge::closeEvent(QCloseEvent* e) {
    if (!m_closing) {
        m_closing = true;
        stopAll();
        emit closedByUser();
    }
    QDialog::closeEvent(e);
}

QString WebRtcScreenShareBridge::html() {
    return QStringLiteral(R"HTML(
<!doctype html>
<html>
<head>
<meta charset="utf-8">
<style>
body{margin:0;background:#0d0e15;color:white;font-family:Arial, sans-serif;}
.wrap{padding:24px}button{background:#7c3aed;color:white;border:0;border-radius:8px;padding:14px 18px;font-weight:bold;cursor:pointer}
video{width:100%;max-height:240px;background:#000;border-radius:12px;margin-top:18px}.log{margin-top:14px;color:#cbd5e1;font-size:13px;white-space:pre-wrap}
</style>
<script src="qrc:///qtwebchannel/qwebchannel.js"></script>
</head>
<body>
<div class="wrap">
<h2>Transmissão WebRTC</h2>
<p>Clique em iniciar e escolha a tela/janela que deseja transmitir.</p>
<button onclick="startCapture()">Iniciar captura de tela</button>
<video id="preview" autoplay muted playsinline></video>
<div class="log" id="log">Aguardando captura...</div>
</div>
<script>
let bridge=null, localStream=null;
const peers = new Map();
const pending = [];
function log(m){ document.getElementById('log').textContent = m; if(bridge) bridge.jsLog(m); }
new QWebChannel(qt.webChannelTransport, ch => { bridge = ch.objects.bridge; log('Bridge pronto.'); });
async function startCapture(){
  try{
    localStream = await navigator.mediaDevices.getDisplayMedia({video:{frameRate:{ideal:15,max:24},width:{ideal:1280},height:{ideal:720}},audio:false});
    document.getElementById('preview').srcObject = localStream;
    localStream.getVideoTracks()[0].onended = () => { if(bridge) bridge.captureStopped(); };
    if(bridge) bridge.captureStarted();
    log('Captura ativa. Viewers pendentes: '+pending.length);
    while(pending.length) createOffer(pending.shift());
  }catch(e){ log('Falha ao capturar: '+e); }
}
function makePeer(to){
  const pc = new RTCPeerConnection({iceServers:[{urls:'stun:stun.l.google.com:19302'}]});
  pc.onicecandidate = ev => { if(ev.candidate && bridge) bridge.sendIce(to, ev.candidate.candidate, ev.candidate.sdpMid || '', ev.candidate.sdpMLineIndex ?? -1); };
  pc.onconnectionstatechange = () => log('Peer '+to+': '+pc.connectionState);
  if(localStream) localStream.getTracks().forEach(tr => pc.addTrack(tr, localStream));
  peers.set(to, pc); return pc;
}
async function createOffer(to){
  if(!localStream){ if(!pending.includes(to)) pending.push(to); log('Viewer '+to+' aguardando captura.'); return; }
  let pc = peers.get(to); if(!pc) pc = makePeer(to);
  const offer = await pc.createOffer({offerToReceiveVideo:false,offerToReceiveAudio:false});
  await pc.setLocalDescription(offer);
  if(bridge) bridge.sendOffer(to, offer.sdp);
}
window.hallaCreateOffer = createOffer;
window.hallaHandleAnswer = async sig => { const pc = peers.get(sig.from); if(pc && sig.sdp) await pc.setRemoteDescription({type:'answer',sdp:sig.sdp}); };
window.hallaHandleIce = async sig => { const pc = peers.get(sig.from); if(pc && sig.candidate) await pc.addIceCandidate({candidate:sig.candidate,sdpMid:sig.sdpMid || '0',sdpMLineIndex:sig.sdpMLineIndex || 0}); };
window.hallaClosePeer = id => { const pc=peers.get(id); if(pc){pc.close();peers.delete(id);} };
window.hallaStopAll = () => { peers.forEach(pc=>pc.close()); peers.clear(); if(localStream){localStream.getTracks().forEach(t=>t.stop()); localStream=null;} };
</script>
</body>
</html>
)HTML");
}

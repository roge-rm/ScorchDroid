// The browser's JniTransport: the same class the phone's Bluetooth link is,
// with a WebSocket where the phone has an RFCOMM socket. engine_jni.cpp
// builds one for startJoinGameBluetooth() and never knows the difference, so
// a browser joins a game through NetBridge exactly as a phone does over
// Bluetooth.
//
// A page can only open connections, never accept them, so this side can
// join but not host. The other end is the dedicated server's WebSocket
// bridge, which turns each message into one of upstream's TCP frames (a
// 4-byte length and the buffer) and back again, so what the server sees is
// byte-for-byte what a phone would have sent it.
//
// Everything here runs on the page's one thread: the socket's callbacks come
// in between frames, and NetBridge only queues what they hand it.

#include "JniTransport.hpp"

#include <android/log.h>
#include <emscripten/emscripten.h>

#define LOG_TAG "ScorchDroidWebSocket"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

JavaVM *g_javaVM = nullptr;

JniTransport *JniTransport::active_ = nullptr;

namespace {
// valid() only asks whether this is set. There's no Kotlin class to find.
_jclass g_webSocketClass;
// Never reused, as BridgeTransport.hpp asks: a late message from an old
// socket must not land in a new game.
unsigned int g_nextPeerId = 1;
} // namespace

// The sockets live on the JavaScript side, keyed by peer id.
EM_JS(int, sd_ws_open, (int peerId, const char *endpoint), {
    let url = UTF8ToString(endpoint).trim();
    if (!url.startsWith('ws:') && !url.startsWith('wss:')) {
        // A bare host or host:port gets the page's own scheme, since an
        // https page can only open wss.
        const secure = typeof location !== 'undefined' && location.protocol === 'https:';
        url = (secure ? 'wss://' : 'ws://') + url;
    }
    let socket;
    try {
        socket = new WebSocket(url);
    } catch (e) {
        console.error('ScorchDroidWebSocket: ' + e);
        return 0;
    }
    socket.binaryType = 'arraybuffer';
    Module.sdSockets = Module.sdSockets || {};
    Module.sdSockets[peerId] = socket;
    let opened = false;
    socket.onopen = () => {
        opened = true;
        Module._sd_ws_on_open(peerId);
    };
    socket.onmessage = (event) => {
        if (!(event.data instanceof ArrayBuffer)) return;
        const bytes = new Uint8Array(event.data);
        if (bytes.length === 0) return;
        const ptr = Module._malloc(bytes.length);
        HEAPU8.set(bytes, ptr);
        Module._sd_ws_on_payload(peerId, ptr, bytes.length);
        Module._free(ptr);
    };
    socket.onclose = () => {
        if (Module.sdSockets[peerId] !== socket) return;
        delete Module.sdSockets[peerId];
        if (opened) {
            Module._sd_ws_on_closed(peerId);
        } else {
            const reason = stringToNewUTF8("couldn't reach " + url);
            Module._sd_ws_on_failed(reason);
            Module._free(reason);
        }
    };
    return 1;
});

EM_JS(int, sd_ws_send, (int peerId, const unsigned char *bytes, int length), {
    const socket = Module.sdSockets && Module.sdSockets[peerId];
    if (!socket || socket.readyState !== 1) return 0;
    socket.send(HEAPU8.slice(bytes, bytes + length));
    return 1;
});

EM_JS(void, sd_ws_close, (int peerId), {
    const socket = Module.sdSockets && Module.sdSockets[peerId];
    if (!socket) return;
    delete Module.sdSockets[peerId];
    socket.close();
});

EM_JS(void, sd_ws_close_all, (), {
    const sockets = Module.sdSockets || {};
    Module.sdSockets = {};
    for (const id in sockets) sockets[id].close();
});

JniTransport::JniTransport(JNIEnv *)
	: transportClass_(&g_webSocketClass)
	, startListeningMethod_(nullptr)
	, connectToMethod_(nullptr)
	, sendMethod_(nullptr)
	, disconnectMethod_(nullptr)
	, stopMethod_(nullptr)
	, sink_(nullptr)
{
	active_ = this;
}

JniTransport::~JniTransport()
{
	stop();
	if (active_ == this) active_ = nullptr;
}

JniTransport *JniTransport::active()
{
	return active_;
}

void JniTransport::setSink(BridgeTransportSink *sink)
{
	sink_ = sink;
}

bool JniTransport::startListening()
{
	// A web page can't accept a connection.
	return false;
}

bool JniTransport::connectTo(const char *endpoint)
{
	const unsigned int peerId = g_nextPeerId++;
	LOGI("connecting to %s as peer %u", endpoint ? endpoint : "", peerId);
	return sd_ws_open((int) peerId, endpoint ? endpoint : "") != 0;
}

bool JniTransport::send(unsigned int peerId, const unsigned char *bytes, unsigned int length)
{
	return sd_ws_send((int) peerId, bytes, (int) length) != 0;
}

void JniTransport::disconnect(unsigned int peerId)
{
	sd_ws_close((int) peerId);
}

void JniTransport::stop()
{
	sd_ws_close_all();
}

void JniTransport::peerConnected(unsigned int peerId)
{
	LOGI("peer %u connected", peerId);
	if (sink_) sink_->onPeerConnected(peerId);
}

void JniTransport::payload(unsigned int peerId, const unsigned char *bytes, unsigned int length)
{
	if (sink_) sink_->onPayload(peerId, bytes, length);
}

void JniTransport::peerDisconnected(unsigned int peerId)
{
	LOGI("peer %u disconnected", peerId);
	if (sink_) sink_->onPeerDisconnected(peerId);
}

void JniTransport::failed(const char *reason)
{
	LOGE("transport failed: %s", reason ? reason : "");
	if (sink_) sink_->onTransportFailed(reason ? reason : "the connection failed");
}

extern "C" {

EMSCRIPTEN_KEEPALIVE void sd_ws_on_open(int peerId)
{
	if (JniTransport *transport = JniTransport::active()) transport->peerConnected((unsigned int) peerId);
}

EMSCRIPTEN_KEEPALIVE void sd_ws_on_payload(int peerId, const unsigned char *bytes, int length)
{
	if (length <= 0) return;
	if (JniTransport *transport = JniTransport::active())
		transport->payload((unsigned int) peerId, bytes, (unsigned int) length);
}

EMSCRIPTEN_KEEPALIVE void sd_ws_on_closed(int peerId)
{
	if (JniTransport *transport = JniTransport::active()) transport->peerDisconnected((unsigned int) peerId);
}

EMSCRIPTEN_KEEPALIVE void sd_ws_on_failed(const char *reason)
{
	if (JniTransport *transport = JniTransport::active()) transport->failed(reason);
}

} // extern "C"

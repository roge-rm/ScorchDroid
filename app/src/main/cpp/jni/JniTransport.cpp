#include "JniTransport.hpp"

#include <android/log.h>
#include <string.h>

#define LOG_TAG "ScorchDroidBridge"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

JavaVM *g_javaVM = nullptr;

JniTransport *JniTransport::active_ = nullptr;

namespace {

// A JNIEnv for whatever thread this is, attaching if it is not a Java thread
// and detaching again on the way out if that is what it took. NetBridge's
// send thread is a plain pthread and has to do this; Kotlin's reader threads
// already have an env and this finds it without touching anything.
class AttachedEnv
{
public:
	AttachedEnv() : env_(nullptr), attached_(false)
	{
		if (!g_javaVM) return;
		if (g_javaVM->GetEnv((void **) &env_, JNI_VERSION_1_6) == JNI_OK) return;
		if (g_javaVM->AttachCurrentThread(&env_, nullptr) == JNI_OK) attached_ = true;
		else env_ = nullptr;
	}

	~AttachedEnv()
	{
		if (attached_ && g_javaVM) g_javaVM->DetachCurrentThread();
	}

	JNIEnv *get() { return env_; }

private:
	JNIEnv *env_;
	bool    attached_;
};

}  // namespace

JniTransport::JniTransport(JNIEnv *env)
	: transportClass_(nullptr)
	, startListeningMethod_(nullptr)
	, connectToMethod_(nullptr)
	, sendMethod_(nullptr)
	, disconnectMethod_(nullptr)
	, stopMethod_(nullptr)
	, sink_(nullptr)
{
	active_ = this;

	jclass localClass = env->FindClass("com/rm/scorchdroid/BluetoothTransport");
	if (!localClass)
	{
		env->ExceptionClear();
		LOGE("BluetoothTransport class not found");
		return;
	}
	// A global ref because the send thread uses this long after the call
	// that built it has returned; a local one is only valid for that call.
	transportClass_ = (jclass) env->NewGlobalRef(localClass);
	env->DeleteLocalRef(localClass);

	startListeningMethod_ = env->GetStaticMethodID(transportClass_, "nativeStartListening", "()Z");
	connectToMethod_ = env->GetStaticMethodID(transportClass_, "nativeConnectTo", "(Ljava/lang/String;)Z");
	sendMethod_ = env->GetStaticMethodID(transportClass_, "nativeSend", "(I[B)Z");
	disconnectMethod_ = env->GetStaticMethodID(transportClass_, "nativeDisconnect", "(I)V");
	stopMethod_ = env->GetStaticMethodID(transportClass_, "nativeStop", "()V");

	if (!startListeningMethod_ || !connectToMethod_ || !sendMethod_ ||
		!disconnectMethod_ || !stopMethod_)
	{
		env->ExceptionClear();
		LOGE("BluetoothTransport is missing one of its native-facing methods");
		env->DeleteGlobalRef(transportClass_);
		transportClass_ = nullptr;
	}
}

JniTransport::~JniTransport()
{
	stop();
	if (transportClass_)
	{
		AttachedEnv env;
		if (env.get()) env.get()->DeleteGlobalRef(transportClass_);
		transportClass_ = nullptr;
	}
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
	if (!valid()) return false;
	AttachedEnv env;
	if (!env.get()) return false;
	return env.get()->CallStaticBooleanMethod(transportClass_, startListeningMethod_) == JNI_TRUE;
}

bool JniTransport::connectTo(const char *endpoint)
{
	if (!valid()) return false;
	AttachedEnv env;
	if (!env.get()) return false;

	jstring address = env.get()->NewStringUTF(endpoint ? endpoint : "");
	jboolean started = env.get()->CallStaticBooleanMethod(transportClass_, connectToMethod_, address);
	env.get()->DeleteLocalRef(address);
	return started == JNI_TRUE;
}

bool JniTransport::send(unsigned int peerId, const unsigned char *bytes, unsigned int length)
{
	if (!valid()) return false;
	AttachedEnv env;
	if (!env.get()) return false;

	jbyteArray array = env.get()->NewByteArray((jsize) length);
	if (!array)
	{
		env.get()->ExceptionClear();
		return false;
	}
	env.get()->SetByteArrayRegion(array, 0, (jsize) length, (const jbyte *) bytes);
	jboolean sent = env.get()->CallStaticBooleanMethod(
		transportClass_, sendMethod_, (jint) peerId, array);
	env.get()->DeleteLocalRef(array);
	return sent == JNI_TRUE;
}

void JniTransport::disconnect(unsigned int peerId)
{
	if (!valid()) return;
	AttachedEnv env;
	if (!env.get()) return;
	env.get()->CallStaticVoidMethod(transportClass_, disconnectMethod_, (jint) peerId);
}

void JniTransport::stop()
{
	if (!valid()) return;
	AttachedEnv env;
	if (!env.get()) return;
	env.get()->CallStaticVoidMethod(transportClass_, stopMethod_);
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
	if (sink_) sink_->onTransportFailed(reason ? reason : "the Bluetooth link failed");
}

// The four ways Kotlin reports what its sockets did. All arrive on its own
// reader/accept threads and hand straight over to NetBridge, which is built
// to be called from exactly there.

extern "C" JNIEXPORT void JNICALL
Java_com_rm_scorchdroid_BluetoothTransport_nativePeerConnected(
	JNIEnv *env, jobject /* this */, jint peerId)
{
	JniTransport *transport = JniTransport::active();
	if (transport) transport->peerConnected((unsigned int) peerId);
}

extern "C" JNIEXPORT void JNICALL
Java_com_rm_scorchdroid_BluetoothTransport_nativePayload(
	JNIEnv *env, jobject /* this */, jint peerId, jbyteArray data, jint length)
{
	JniTransport *transport = JniTransport::active();
	if (!transport || length <= 0) return;

	jbyte *bytes = env->GetByteArrayElements(data, nullptr);
	if (!bytes) return;
	transport->payload((unsigned int) peerId, (const unsigned char *) bytes, (unsigned int) length);
	// JNI_ABORT: nothing was written back, so there is no copy to commit.
	env->ReleaseByteArrayElements(data, bytes, JNI_ABORT);
}

extern "C" JNIEXPORT void JNICALL
Java_com_rm_scorchdroid_BluetoothTransport_nativePeerDisconnected(
	JNIEnv *env, jobject /* this */, jint peerId)
{
	JniTransport *transport = JniTransport::active();
	if (transport) transport->peerDisconnected((unsigned int) peerId);
}

extern "C" JNIEXPORT void JNICALL
Java_com_rm_scorchdroid_BluetoothTransport_nativeFailed(
	JNIEnv *env, jobject /* this */, jstring jReason)
{
	JniTransport *transport = JniTransport::active();
	if (!transport) return;

	const char *reason = jReason ? env->GetStringUTFChars(jReason, nullptr) : nullptr;
	transport->failed(reason);
	if (reason) env->ReleaseStringUTFChars(jReason, reason);
}

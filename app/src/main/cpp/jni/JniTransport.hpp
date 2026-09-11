#ifndef SCORCHDROID_JNI_TRANSPORT_HPP
#define SCORCHDROID_JNI_TRANSPORT_HPP

#include <BridgeTransport.hpp>
#include <jni.h>

/**
 * The Android half of the Bluetooth link: a BridgeTransport whose sockets
 * live in Kotlin (BluetoothTransport.kt) and whose threads are Java threads.
 *
 * All it does is cross the JNI boundary in both directions. Outbound calls
 * come from NetBridge's send thread, which is a bare pthread with no JNIEnv
 * of its own, so each one attaches to the VM for the duration - a cost that
 * does not matter at the one kilobit a second this game actually sends.
 * Inbound calls arrive on Kotlin's reader threads, which are already Java
 * threads, and go straight to the sink.
 *
 * One at a time: the engine hosts or joins, never both, and a single static
 * instance is what the JNI callbacks find their way back to.
 */
class JniTransport : public BridgeTransport
{
public:
	/** [env] is only used to look up the Kotlin object, not remembered. */
	JniTransport( JNIEnv* env );
	virtual ~JniTransport();

	/** Whether the Kotlin side could be found at all. */
	bool valid() const { return transportClass_ != nullptr; }

	// BridgeTransport
	virtual void setSink( BridgeTransportSink* sink );
	virtual bool startListening();
	virtual bool connectTo( const char* endpoint );
	virtual bool send( unsigned int peerId, const unsigned char* bytes, unsigned int length );
	virtual void disconnect( unsigned int peerId );
	virtual void stop();

	// Called from the JNI entry points below, on Kotlin's own threads.
	static JniTransport* active();
	void peerConnected( unsigned int peerId );
	void payload( unsigned int peerId, const unsigned char* bytes, unsigned int length );
	void peerDisconnected( unsigned int peerId );
	void failed( const char* reason );

private:
	jclass    transportClass_;
	jmethodID startListeningMethod_;
	jmethodID connectToMethod_;
	jmethodID sendMethod_;
	jmethodID disconnectMethod_;
	jmethodID stopMethod_;

	BridgeTransportSink* sink_;

	static JniTransport* active_;
};

/** Cached by JNI_OnLoad; the only way a non-Java thread can reach the VM. */
extern JavaVM* g_javaVM;

#endif  // SCORCHDROID_JNI_TRANSPORT_HPP

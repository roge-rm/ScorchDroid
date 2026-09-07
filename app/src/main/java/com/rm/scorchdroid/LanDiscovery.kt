package com.rm.scorchdroid

import android.content.Context
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import android.net.wifi.WifiManager
import android.util.Log

/**
 * M5: Android-to-Android LAN discovery via NSD/mDNS (see the porting plan).
 * Purely a rendezvous mechanism, wrapping the standard `NsdManager` APIs -
 * actual gameplay traffic is the real `NetServerTCP3` socket
 * (engine_jni.cpp's startLocalGame) once a host/port is known, whether that
 * came from this discovery or a manually typed IP. Kept as new, standalone
 * app code (not a native/JNI concern) since NSD is a plain Android
 * platform API with nothing to do with the game engine itself.
 */
object LanDiscovery {
    private const val TAG = "LanDiscovery"
    const val SERVICE_TYPE = "_scorchdroid._tcp."

    data class FoundGame(val name: String, val host: String, val port: Int)

    private var nsdManager: NsdManager? = null
    private var registrationListener: NsdManager.RegistrationListener? = null
    private var discoveryListener: NsdManager.DiscoveryListener? = null
    private var multicastLock: WifiManager.MulticastLock? = null

    /**
     * Advertises this device's hosted game so other ScorchDroid devices on
     * the same LAN can find it via [startDiscovery]. Safe to call
     * repeatedly - stops any previous registration first.
     */
    fun registerService(context: Context, port: Int) {
        stopRegistration()
        acquireMulticastLock(context)

        val manager = (context.getSystemService(Context.NSD_SERVICE) as NsdManager).also { nsdManager = it }
        val serviceInfo = NsdServiceInfo().apply {
            serviceName = "ScorchDroid-${android.os.Build.MODEL}"
            serviceType = SERVICE_TYPE
            setPort(port)
        }

        val listener = object : NsdManager.RegistrationListener {
            override fun onServiceRegistered(info: NsdServiceInfo) {
                Log.i(TAG, "Registered LAN service: ${info.serviceName}")
            }
            override fun onRegistrationFailed(info: NsdServiceInfo, errorCode: Int) {
                Log.e(TAG, "Failed to register LAN service (error $errorCode)")
            }
            override fun onServiceUnregistered(info: NsdServiceInfo) {}
            override fun onUnregistrationFailed(info: NsdServiceInfo, errorCode: Int) {}
        }
        registrationListener = listener
        manager.registerService(serviceInfo, NsdManager.PROTOCOL_DNS_SD, listener)
    }

    fun stopRegistration() {
        registrationListener?.let { listener ->
            try {
                nsdManager?.unregisterService(listener)
            } catch (e: IllegalArgumentException) {
                // Already unregistered/never succeeded - fine to ignore.
            }
        }
        registrationListener = null
        releaseMulticastLock()
    }

    /**
     * Discovers other ScorchDroid games on the LAN for [durationMs], then
     * stops itself automatically - a bounded scan for a "Find Games"
     * button's dialog, not a persistent background scan. [onFound] may be
     * called multiple times as services resolve (each resolution is its
     * own async NSD callback).
     */
    fun startDiscovery(context: Context, durationMs: Long, onFound: (FoundGame) -> Unit, onFinished: () -> Unit) {
        stopDiscovery()
        acquireMulticastLock(context)

        val manager = (context.getSystemService(Context.NSD_SERVICE) as NsdManager).also { nsdManager = it }
        val mainHandler = android.os.Handler(android.os.Looper.getMainLooper())

        val listener = object : NsdManager.DiscoveryListener {
            override fun onDiscoveryStarted(serviceType: String) {
                Log.i(TAG, "LAN discovery started")
            }
            override fun onServiceFound(service: NsdServiceInfo) {
                manager.resolveService(service, object : NsdManager.ResolveListener {
                    override fun onResolveFailed(info: NsdServiceInfo, errorCode: Int) {
                        Log.e(TAG, "Failed to resolve ${info.serviceName} (error $errorCode)")
                    }
                    override fun onServiceResolved(info: NsdServiceInfo) {
                        val host = info.host?.hostAddress ?: return
                        mainHandler.post { onFound(FoundGame(info.serviceName, host, info.port)) }
                    }
                })
            }
            override fun onServiceLost(service: NsdServiceInfo) {}
            override fun onDiscoveryStopped(serviceType: String) {}
            override fun onStartDiscoveryFailed(serviceType: String, errorCode: Int) {
                Log.e(TAG, "Failed to start LAN discovery (error $errorCode)")
                mainHandler.post { onFinished() }
            }
            override fun onStopDiscoveryFailed(serviceType: String, errorCode: Int) {}
        }
        discoveryListener = listener
        manager.discoverServices(SERVICE_TYPE, NsdManager.PROTOCOL_DNS_SD, listener)

        mainHandler.postDelayed({
            stopDiscovery()
            onFinished()
        }, durationMs)
    }

    fun stopDiscovery() {
        discoveryListener?.let { listener ->
            try {
                nsdManager?.stopServiceDiscovery(listener)
            } catch (e: IllegalArgumentException) {
                // Already stopped - fine to ignore.
            }
        }
        discoveryListener = null
        releaseMulticastLock()
    }

    // NSD's mDNS traffic is multicast - some OEM Wi-Fi stacks drop
    // multicast packets to save power unless something explicitly asks to
    // keep receiving them, which manifests as NSD registration/discovery
    // silently failing to see anything. Called by both registerService()
    // and startDiscovery() since either side benefits from holding it, and
    // released once both are idle (checked at release time).
    private fun acquireMulticastLock(context: Context) {
        if (multicastLock?.isHeld == true) return
        val wifiManager = context.applicationContext.getSystemService(Context.WIFI_SERVICE) as? WifiManager ?: return
        multicastLock = wifiManager.createMulticastLock("scorchdroid-nsd").apply {
            setReferenceCounted(false)
            acquire()
        }
    }

    private fun releaseMulticastLock() {
        if (registrationListener != null || discoveryListener != null) return
        multicastLock?.let { if (it.isHeld) it.release() }
        multicastLock = null
    }
}

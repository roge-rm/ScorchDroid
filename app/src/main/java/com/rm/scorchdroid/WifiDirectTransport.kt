package com.rm.scorchdroid

import android.Manifest
import android.annotation.SuppressLint
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.net.wifi.WpsInfo
import android.net.wifi.p2p.WifiP2pConfig
import android.net.wifi.p2p.WifiP2pManager
import android.net.wifi.p2p.nsd.WifiP2pDnsSdServiceInfo
import android.net.wifi.p2p.nsd.WifiP2pDnsSdServiceRequest
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.util.Log
import androidx.core.content.ContextCompat
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlin.coroutines.resume

/**
 * Peer-to-peer play with no router, no hotspot and no internet: Wi-Fi Direct.
 *
 * The point of this file is what it *doesn't* do. Wi-Fi Direct forms a real
 * IPv4 network between the devices - the group owner sits at a fixed address
 * (conventionally 192.168.49.1) and everyone else gets a DHCP lease from it -
 * so once the group is up, the game underneath is the same `NetServerTCP3`
 * socket path a LAN game already uses. The engine is untouched by any of
 * this; like [LanDiscovery] this is a rendezvous mechanism and nothing more.
 *
 * The split of responsibilities mirrors [LanDiscovery] deliberately, and the
 * two advertise the same `_scorchdroid._tcp` service name, because to a
 * player they are one feature - "find someone to play with" - and only differ
 * in which radio carries it.
 *
 * Structured as a host that owns its group: [advertise] calls `createGroup`
 * rather than waiting to be invited, so the hosting device is always the
 * group owner and always has the address the client needs. The alternative
 * (negotiating a group at connect time) leaves which device ends up owning it
 * to a bidding process, and a host that turns out not to be the owner has no
 * address to publish.
 */
object WifiDirectTransport {
    private const val TAG = "WifiDirectTransport"

    // Wi-Fi P2P's DNS-SD wants the bare service type; NSD wants it fully
    // qualified. Derived from LanDiscovery's rather than written out twice,
    // so the two can never drift into advertising different services.
    private val SERVICE_TYPE = LanDiscovery.SERVICE_TYPE.trimEnd('.')

    // The TXT record carries the port, because a Wi-Fi Direct service
    // response tells us the device but not what it's listening on.
    private const val TXT_PORT = "port"
    private const val TXT_NAME = "name"

    private var manager: WifiP2pManager? = null
    private var channel: WifiP2pManager.Channel? = null
    private var serviceRequest: WifiP2pDnsSdServiceRequest? = null
    private var advertising = false
    private var connectReceiver: BroadcastReceiver? = null
    private var discoveryTimeout: Runnable? = null

    /**
     * The permissions Wi-Fi Direct needs at runtime, which changed shape in
     * API 33. Before that, peer discovery was treated as a location capability
     * and needed the full location permission; from 33 there is a dedicated
     * `NEARBY_WIFI_DEVICES` that we can (and do - see the manifest) declare
     * with `neverForLocation`, so the game never asks for location at all on a
     * modern device.
     */
    fun requiredPermissions(): Array<String> =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            arrayOf(Manifest.permission.NEARBY_WIFI_DEVICES)
        } else {
            arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)
        }

    fun hasPermissions(context: Context): Boolean =
        requiredPermissions().all {
            ContextCompat.checkSelfPermission(context, it) == PackageManager.PERMISSION_GRANTED
        }

    /**
     * Whether this device can do Wi-Fi Direct at all. Declared
     * `required="false"` in the manifest so the game still installs on
     * hardware without it, which means every entry point has to check.
     */
    fun isSupported(context: Context): Boolean =
        context.packageManager.hasSystemFeature(PackageManager.FEATURE_WIFI_DIRECT) &&
            context.getSystemService(Context.WIFI_P2P_SERVICE) != null

    private fun ensureChannel(context: Context): Boolean {
        if (channel != null) return true
        val service = context.applicationContext.getSystemService(Context.WIFI_P2P_SERVICE)
            as? WifiP2pManager ?: return false
        manager = service
        // The framework drops the channel if the Wi-Fi Direct stack restarts;
        // null it out so the next call rebuilds rather than issuing requests
        // against a dead one and silently getting nothing back.
        channel = service.initialize(context.applicationContext, Looper.getMainLooper()) {
            Log.w(TAG, "Wi-Fi Direct channel disconnected")
            channel = null
        }
        return channel != null
    }

    // The framework's own callback shape, which every call below needs and
    // none of them need differently. The reason is passed on rather than
    // flattened away because BUSY - "you are already doing this" - is a
    // success for some callers and a failure for others.
    private fun actionListener(what: String, onResult: (Boolean, Int) -> Unit = { _, _ -> }) =
        object : WifiP2pManager.ActionListener {
            override fun onSuccess() {
                Log.i(TAG, "$what succeeded")
                onResult(true, -1)
            }

            override fun onFailure(reason: Int) {
                Log.e(TAG, "$what failed (reason $reason)")
                onResult(false, reason)
            }
        }

    /**
     * Makes this device a Wi-Fi Direct group owner and advertises the hosted
     * game on it, so a peer running [startDiscovery] can find and join it with
     * no shared network of any kind.
     *
     * [onResult] reports whether the group actually formed - a caller that
     * assumed success would show the player a "ready to play" state for a
     * group that never came up.
     */
    @SuppressLint("MissingPermission")  // Guarded by hasPermissions below.
    fun advertise(context: Context, port: Int, onResult: (Boolean) -> Unit = {}) {
        if (!isSupported(context) || !hasPermissions(context) || !ensureChannel(context)) {
            onResult(false)
            return
        }
        val mgr = manager ?: return onResult(false)
        val ch = channel ?: return onResult(false)

        val record = mapOf(
            TXT_PORT to port.toString(),
            TXT_NAME to android.os.Build.MODEL,
        )
        val serviceInfo = WifiP2pDnsSdServiceInfo.newInstance(
            "ScorchDroid-${android.os.Build.MODEL}", SERVICE_TYPE, record
        )

        // Clear first: re-hosting without this stacks a second identical
        // service record on the old one, and peers then see the game twice.
        mgr.clearLocalServices(ch, actionListener("clearLocalServices") { _, _ ->
            mgr.addLocalService(ch, serviceInfo, actionListener("addLocalService") { added, _ ->
                if (!added) return@actionListener onResult(false)
                // A group the host owns outright, rather than one negotiated
                // at connect time - see this object's header for why.
                mgr.createGroup(ch, actionListener("createGroup") { created, reason ->
                    // BUSY means a group is already up - typically this app's
                    // own, from a previous game - which is what the caller
                    // wanted. Any other failure is a failure: a host told it
                    // is reachable over Wi-Fi Direct when no group formed
                    // would sit waiting for peers that can never arrive.
                    val up = created || reason == WifiP2pManager.BUSY
                    advertising = up
                    onResult(up)
                })
            })
        })
    }

    /** Whether this device is currently hosting a Wi-Fi Direct group. */
    fun isAdvertising(): Boolean = advertising

    @SuppressLint("MissingPermission")  // Guarded by hasPermissions below.
    fun stopAdvertising(context: Context) {
        val mgr = manager ?: return
        val ch = channel ?: return
        if (!advertising) return
        advertising = false
        if (!hasPermissions(context)) return
        mgr.clearLocalServices(ch, actionListener("clearLocalServices"))
        mgr.removeGroup(ch, actionListener("removeGroup"))
    }

    /**
     * Looks for games being advertised over Wi-Fi Direct for [durationMs],
     * then stops itself - the same bounded-scan shape as
     * [LanDiscovery.startDiscovery], so the "Find Games" dialog can run both
     * at once and merge what they turn up.
     *
     * Results arrive as [LanDiscovery.FoundGame] with a `p2pDeviceAddress`
     * and no usable host: there is no IP until a group has been formed, which
     * is what [connectToOwner] does when the player picks one.
     */
    @SuppressLint("MissingPermission")  // Guarded by hasPermissions below.
    fun startDiscovery(
        context: Context,
        durationMs: Long,
        onFound: (LanDiscovery.FoundGame) -> Unit,
        onFinished: () -> Unit,
    ) {
        if (!isSupported(context) || !hasPermissions(context) || !ensureChannel(context)) {
            onFinished()
            return
        }
        val mgr = manager ?: return onFinished()
        val ch = channel ?: return onFinished()
        stopDiscovery()

        val mainHandler = Handler(Looper.getMainLooper())
        // The two listeners fire separately for the same service - the TXT
        // record carries the port, the service response carries the readable
        // name - so hold the ports until the matching response arrives.
        val portsByDevice = mutableMapOf<String, Int>()
        val namesByDevice = mutableMapOf<String, String>()

        fun emit(deviceAddress: String) {
            val port = portsByDevice[deviceAddress] ?: return
            val name = namesByDevice[deviceAddress] ?: return
            mainHandler.post {
                onFound(
                    LanDiscovery.FoundGame(
                        name = name,
                        host = "",
                        port = port,
                        p2pDeviceAddress = deviceAddress,
                    )
                )
            }
        }

        mgr.setDnsSdResponseListeners(
            ch,
            { instanceName, _, device ->
                namesByDevice[device.deviceAddress] =
                    device.deviceName.ifEmpty { instanceName }
                emit(device.deviceAddress)
            },
            { _, record, device ->
                record[TXT_PORT]?.toIntOrNull()?.let { portsByDevice[device.deviceAddress] = it }
                record[TXT_NAME]?.let { namesByDevice.putIfAbsent(device.deviceAddress, it) }
                emit(device.deviceAddress)
            },
        )

        val request = WifiP2pDnsSdServiceRequest.newInstance(SERVICE_TYPE)
        serviceRequest = request
        mgr.addServiceRequest(ch, request, actionListener("addServiceRequest") { added, _ ->
            if (!added) {
                mainHandler.post { onFinished() }
                return@actionListener
            }
            mgr.discoverServices(ch, actionListener("discoverServices") { started, _ ->
                if (!started) mainHandler.post { onFinished() }
            })
        })

        discoveryTimeout = Runnable {
            discoveryTimeout = null
            stopDiscovery()
            onFinished()
        }
        mainHandler.postDelayed(discoveryTimeout!!, durationMs)
    }

    @SuppressLint("MissingPermission")
    fun stopDiscovery() {
        // Drop the pending timeout as well as the scan itself. Without this a
        // scan the player cancelled still reports finishing, seconds later,
        // to a dialog that has already been dismissed.
        discoveryTimeout?.let { Handler(Looper.getMainLooper()).removeCallbacks(it) }
        discoveryTimeout = null
        val mgr = manager ?: return
        val ch = channel ?: return
        serviceRequest?.let { mgr.removeServiceRequest(ch, it, actionListener("removeServiceRequest")) }
        serviceRequest = null
    }

    /**
     * Joins [deviceAddress]'s group and answers with the group owner's IP -
     * the address the existing `NativeBridge.startJoinGame` path then dials,
     * exactly as if it had come from NSD.
     *
     * Suspends because group formation genuinely takes seconds and shows the
     * peer a system invitation prompt; the caller needs to be able to show
     * that as its own state and to cancel it. Answers null on timeout or
     * failure rather than throwing, since every caller's response to "no
     * group" is the same.
     */
    @SuppressLint("MissingPermission")  // Guarded by hasPermissions below.
    suspend fun connectToOwner(
        context: Context,
        deviceAddress: String,
        timeoutMs: Long = 30_000,
    ): String? {
        if (!isSupported(context) || !hasPermissions(context) || !ensureChannel(context)) return null
        val mgr = manager ?: return null
        val ch = channel ?: return null

        return suspendCancellableCoroutine { continuation ->
            val mainHandler = Handler(Looper.getMainLooper())
            var settled = false

            fun finish(address: String?) {
                if (settled) return
                settled = true
                unregisterConnectReceiver(context)
                mainHandler.removeCallbacksAndMessages(null)
                if (continuation.isActive) continuation.resume(address)
            }

            val receiver = object : BroadcastReceiver() {
                override fun onReceive(ctx: Context, intent: Intent) {
                    if (intent.action != WifiP2pManager.WIFI_P2P_CONNECTION_CHANGED_ACTION) return
                    // Ask rather than reading the intent's extra: the extra is
                    // only populated on some versions, the request never isn't.
                    mgr.requestConnectionInfo(ch) { info ->
                        if (info != null && info.groupFormed) {
                            finish(info.groupOwnerAddress?.hostAddress)
                        }
                    }
                }
            }
            connectReceiver = receiver
            ContextCompat.registerReceiver(
                context.applicationContext,
                receiver,
                IntentFilter(WifiP2pManager.WIFI_P2P_CONNECTION_CHANGED_ACTION),
                ContextCompat.RECEIVER_NOT_EXPORTED,
            )

            val config = WifiP2pConfig().apply {
                this.deviceAddress = deviceAddress
                wps.setup = WpsInfo.PBC
                // The host already owns a group (see advertise), so decline to
                // own one - a tie here leaves nobody at a known address.
                groupOwnerIntent = 0
            }
            mgr.connect(ch, config, actionListener("connect") { requested, _ ->
                if (!requested) finish(null)
            })

            mainHandler.postDelayed({
                Log.e(TAG, "Wi-Fi Direct group did not form within ${timeoutMs}ms")
                finish(null)
            }, timeoutMs)

            continuation.invokeOnCancellation { finish(null) }
        }
    }

    private fun unregisterConnectReceiver(context: Context) {
        connectReceiver?.let {
            try {
                context.applicationContext.unregisterReceiver(it)
            } catch (e: IllegalArgumentException) {
                // Never registered, or already gone - nothing to undo.
            }
        }
        connectReceiver = null
    }

    /** Tears down any group this device is in - the counterpart to a join. */
    @SuppressLint("MissingPermission")
    fun disconnect(context: Context) {
        unregisterConnectReceiver(context)
        val mgr = manager ?: return
        val ch = channel ?: return
        if (hasPermissions(context)) mgr.removeGroup(ch, actionListener("removeGroup"))
    }
}

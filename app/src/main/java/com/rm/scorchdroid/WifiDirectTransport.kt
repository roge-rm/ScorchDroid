package com.rm.scorchdroid

import android.Manifest
import android.annotation.SuppressLint
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.location.LocationManager
import android.net.wifi.WifiManager
import android.net.wifi.WpsInfo
import android.net.wifi.p2p.WifiP2pConfig
import android.net.wifi.p2p.WifiP2pDevice
import android.net.wifi.p2p.WifiP2pManager
import android.net.wifi.p2p.nsd.WifiP2pDnsSdServiceInfo
import android.net.wifi.p2p.nsd.WifiP2pDnsSdServiceRequest
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.util.Log
import androidx.core.content.ContextCompat
import androidx.core.location.LocationManagerCompat
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
 *
 * The price of that, and it is not obvious: a device that owns a group cannot
 * join anyone else's. `connect()` from inside a group sends an *invitation*
 * instead of joining, so two phones that have each hosted will sit inviting
 * each other until both time out. Groups also outlive the game that made them
 * and the process that asked for one. [connectToOwner] therefore leaves this
 * device's own group before it does anything else.
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
    private var peersReceiver: BroadcastReceiver? = null
    private var rescan: Runnable? = null
    private var appContextForReceiver: Context? = null
    private val mainHandler = Handler(Looper.getMainLooper())

    // How often a scan re-issues its service query while the dialog is up.
    private const val RESCAN_INTERVAL_MS = 5_000L

    // How long to wait for this device's own group to go away before giving
    // up on joining someone else's, and how often to look.
    private const val GROUP_TEARDOWN_TIMEOUT_MS = 6_000L
    private const val GROUP_TEARDOWN_POLL_MS = 500L

    // Kept apart from [mainHandler]: connecting cancels its own pending work
    // with removeCallbacksAndMessages(null), which would otherwise take a
    // running scan's retries down with it.
    private val connectHandler = Handler(Looper.getMainLooper())

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
     * Why Wi-Fi Direct cannot be used right now, phrased for a player to act
     * on, or null if it can.
     *
     * Every entry point here used to fail silently on all four of these, and
     * they are not distinguishable from the outside: a refused permission, a
     * switched-off radio and simply nobody being there all looked identical -
     * an empty "Find Games" list. That is what made the first two-device test
     * uninformative, so the reason is now something callers can put on
     * screen. None of these is a fault in the radio; each is a different
     * thing for the player to go and change.
     */
    fun unavailableReason(context: Context): String? {
        if (!isSupported(context)) return "this device has no Wi-Fi Direct"
        if (!hasPermissions(context)) {
            return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                "the nearby devices permission was refused"
            } else {
                "the location permission was refused - Android ${Build.VERSION.RELEASE} " +
                    "treats finding nearby devices as a location feature"
            }
        }
        // Wi-Fi P2P rides the Wi-Fi radio: the point of it is not needing a
        // *network*, but the radio itself still has to be on.
        if (!isWifiEnabled(context)) {
            return "Wi-Fi is switched off - turn it on, it does not need to join a network"
        }
        // Below API 33 the permission is not enough on its own: with the
        // master location toggle off, discovery succeeds and then reports
        // nothing, forever. From 33 NEARBY_WIFI_DEVICES covers it and the
        // toggle is irrelevant, which is why this is version-gated rather
        // than asked of everyone.
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU && !isLocationEnabled(context)) {
            return "Location is switched off in system settings, which Android " +
                "${Build.VERSION.RELEASE} needs before it will look for nearby devices"
        }
        return null
    }

    private fun isWifiEnabled(context: Context): Boolean {
        val wifi = context.applicationContext.getSystemService(Context.WIFI_SERVICE) as? WifiManager
        return wifi?.isWifiEnabled ?: false
    }

    private fun isLocationEnabled(context: Context): Boolean {
        val lm = context.applicationContext.getSystemService(Context.LOCATION_SERVICE)
            as? LocationManager ?: return false
        return LocationManagerCompat.isLocationEnabled(lm)
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
    @SuppressLint("MissingPermission")  // Guarded by unavailableReason below.
    fun advertise(context: Context, port: Int, onResult: (Boolean, String?) -> Unit = { _, _ -> }) {
        val reason = unavailableReason(context)
        if (reason != null) {
            Log.w(TAG, "not advertising: $reason")
            onResult(false, reason)
            return
        }
        if (!ensureChannel(context)) return onResult(false, "the Wi-Fi Direct service did not start")
        val mgr = manager ?: return onResult(false, "no Wi-Fi Direct service")
        val ch = channel ?: return onResult(false, "no Wi-Fi Direct channel")

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
                if (!added) return@actionListener onResult(false, "the game could not be advertised")
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
                    // A host that has a group is also the one device that
                    // other phones have to be able to *see*, so say what the
                    // group actually came up as - band and owner - rather
                    // than only that it exists. A group owner sitting on a
                    // 5GHz channel is findable in theory and frequently not
                    // in practice, and until this line existed there was no
                    // way to tell that had happened.
                    if (up) logGroupInfo(context)
                    onResult(up, if (up) null else "the Wi-Fi Direct group would not form")
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
     *
     * [onPeerSeen] reports a device the radio can see but which answered no
     * service query. That is a different failure from finding nothing at all
     * - the phones are in range of each other and it is DNS-SD over P2P that
     * is not working - and telling them apart is most of the diagnosis when
     * this does not work on a given pair of handsets.
     */
    @SuppressLint("MissingPermission")  // Guarded by unavailableReason below.
    fun startDiscovery(
        context: Context,
        durationMs: Long,
        onFound: (LanDiscovery.FoundGame) -> Unit,
        onPeerSeen: (name: String, deviceAddress: String) -> Unit = { _, _ -> },
        onFinished: () -> Unit,
    ) {
        val reason = unavailableReason(context)
        if (reason != null) {
            Log.w(TAG, "not scanning: $reason")
            onFinished()
            return
        }
        if (!ensureChannel(context)) return onFinished()
        val mgr = manager ?: return onFinished()
        val ch = channel ?: return onFinished()
        stopDiscovery()

        val appContext = context.applicationContext
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

        // Every DNS-SD answer is logged before it is filtered. A response
        // for the wrong service type still proves the mechanism works on
        // this pair of devices, which is worth knowing when ours is the one
        // that never arrives.
        mgr.setDnsSdResponseListeners(
            ch,
            { instanceName, registrationType, device ->
                Log.i(TAG, "service: $instanceName $registrationType from ${device.deviceAddress}")
                if (!registrationType.contains(SERVICE_TYPE, ignoreCase = true)) return@setDnsSdResponseListeners
                namesByDevice[device.deviceAddress] =
                    device.deviceName.ifEmpty { instanceName }
                emit(device.deviceAddress)
            },
            { fullDomain, record, device ->
                Log.i(TAG, "txt: $fullDomain $record from ${device.deviceAddress}")
                if (!fullDomain.contains(SERVICE_TYPE, ignoreCase = true)) return@setDnsSdResponseListeners
                record[TXT_PORT]?.toIntOrNull()?.let { portsByDevice[device.deviceAddress] = it }
                record[TXT_NAME]?.let { namesByDevice.putIfAbsent(device.deviceAddress, it) }
                emit(device.deviceAddress)
            },
        )

        // Watch plain peer discovery alongside the service query, and report
        // what it sees. See [onPeerSeen]: this is the one signal that
        // separates "the other phone is not there" from "the other phone is
        // there and the service query is going unanswered".
        val peers = object : BroadcastReceiver() {
            override fun onReceive(ctx: Context, intent: Intent) {
                if (intent.action != WifiP2pManager.WIFI_P2P_PEERS_CHANGED_ACTION) return
                if (unavailableReason(appContext) != null) return
                mgr.requestPeers(ch) { list ->
                    list.deviceList.forEach { device ->
                        Log.i(TAG, "peer: ${device.deviceName} ${device.deviceAddress} " +
                            "status=${peerStatus(device)}")
                        val name = device.deviceName.ifEmpty { device.deviceAddress }
                        onPeerSeen(name, device.deviceAddress)
                    }
                }
            }
        }
        peersReceiver = peers
        appContextForReceiver = appContext
        ContextCompat.registerReceiver(
            appContext,
            peers,
            IntentFilter(WifiP2pManager.WIFI_P2P_PEERS_CHANGED_ACTION),
            ContextCompat.RECEIVER_NOT_EXPORTED,
        )

        // Unfiltered, where this used to ask for `SERVICE_TYPE`. A typed
        // request has the supplicant do the matching, and on several stacks
        // a typed request is answered with nothing at all while an untyped
        // one returns the very same service - so ask for everything and
        // match in the listeners above, which costs one string compare.
        val request = WifiP2pDnsSdServiceRequest.newInstance()
        serviceRequest = request
        mgr.addServiceRequest(ch, request, actionListener("addServiceRequest") { added, _ ->
            if (!added) {
                mainHandler.post { onFinished() }
                return@actionListener
            }
            scan(mgr, ch)
        })

        // A service query is a single round of probes: a peer whose radio was
        // elsewhere for that round is simply missed, and nothing retries. So
        // re-issue for as long as the dialog is up rather than concluding
        // from one attempt that nobody is there.
        rescan = object : Runnable {
            override fun run() {
                if (serviceRequest == null) return
                scan(mgr, ch)
                mainHandler.postDelayed(this, RESCAN_INTERVAL_MS)
            }
        }
        mainHandler.postDelayed(rescan!!, RESCAN_INTERVAL_MS)

        discoveryTimeout = Runnable {
            discoveryTimeout = null
            stopDiscovery()
            onFinished()
        }
        mainHandler.postDelayed(discoveryTimeout!!, durationMs)
    }

    /**
     * One round of looking. Peer discovery is started as well as the service
     * query, and deliberately not instead of it: a device that is not running
     * peer discovery does not answer other devices' probes either, so this is
     * also what makes *this* phone findable while it is searching.
     */
    @SuppressLint("MissingPermission")  // Only called from guarded paths.
    private fun scan(mgr: WifiP2pManager, ch: WifiP2pManager.Channel) {
        mgr.discoverPeers(ch, actionListener("discoverPeers"))
        mgr.discoverServices(ch, actionListener("discoverServices"))
    }

    private fun peerStatus(device: WifiP2pDevice): String = when (device.status) {
        WifiP2pDevice.CONNECTED -> "connected"
        WifiP2pDevice.INVITED -> "invited"
        WifiP2pDevice.FAILED -> "failed"
        WifiP2pDevice.AVAILABLE -> "available"
        WifiP2pDevice.UNAVAILABLE -> "unavailable"
        else -> "unknown(${device.status})"
    }

    /**
     * Logs what the group this device owns actually came up as. Only ever a
     * diagnostic: a group on a 5GHz channel is findable in theory and often
     * is not in practice, and without this there is no way to know that is
     * what happened.
     */
    @SuppressLint("MissingPermission")  // Only called once a group has formed.
    private fun logGroupInfo(context: Context) {
        val mgr = manager ?: return
        val ch = channel ?: return
        if (unavailableReason(context) != null) return
        mgr.requestGroupInfo(ch) { group ->
            if (group == null) {
                Log.w(TAG, "group formed but requestGroupInfo returned nothing")
                return@requestGroupInfo
            }
            val frequency = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                group.frequency
            } else {
                0
            }
            Log.i(
                TAG,
                "group up: ssid=${group.networkName} owner=${group.isGroupOwner} " +
                    "frequency=${frequency}MHz interface=${group.`interface`} " +
                    "clients=${group.clientList.size}"
            )
        }
    }

    @SuppressLint("MissingPermission")
    fun stopDiscovery() {
        // Drop the pending timeout and the retry as well as the scan itself.
        // Without this a scan the player cancelled still reports finishing,
        // seconds later, to a dialog that has already been dismissed.
        discoveryTimeout?.let { mainHandler.removeCallbacks(it) }
        discoveryTimeout = null
        rescan?.let { mainHandler.removeCallbacks(it) }
        rescan = null
        peersReceiver?.let { receiver ->
            peersReceiver = null
            appContextForReceiver?.let { ctx ->
                try {
                    ctx.unregisterReceiver(receiver)
                } catch (e: IllegalArgumentException) {
                    // Already gone - nothing to undo.
                }
            }
        }
        val mgr = manager ?: return
        val ch = channel ?: return
        serviceRequest?.let { mgr.removeServiceRequest(ch, it, actionListener("removeServiceRequest")) }
        serviceRequest = null
        // Leave the radio alone once nobody is looking: peer discovery left
        // running is a steady drain and keeps the Wi-Fi chip scanning.
        if (!advertising) {
            try {
                mgr.stopPeerDiscovery(ch, actionListener("stopPeerDiscovery"))
            } catch (e: SecurityException) {
                // Permission revoked mid-scan; the scan is over either way.
            }
        }
    }

    /**
     * The outcome of [connectToOwner]: the group owner's address, or why
     * there isn't one. The reason is carried rather than logged because the
     * player is the one who has to act on most of these - accept a prompt on
     * the other phone, leave a group, turn something on.
     */
    data class ConnectResult(val address: String?, val error: String?)

    /**
     * Joins [deviceAddress]'s group and answers with the group owner's IP -
     * the address the existing `NativeBridge.startJoinGame` path then dials,
     * exactly as if it had come from NSD.
     *
     * **Leaves this device's own group first**, which is the whole reason
     * the first working discovery still could not connect. `connect()` does
     * not mean the same thing to a device that is already in a group:
     * platform documentation for it is explicit that "if the current device
     * is part of an existing p2p group or has created a p2p group with
     * createGroup, an invitation to join the group is sent to the peer
     * device". Two devices that have each hosted - and a device that hosted
     * once and still owns the group, which outlives the game and even the
     * process - therefore do not join each other at all: each sends the
     * other an invitation, neither is joining anything, and both sides time
     * out saying no group could be formed.
     *
     * Suspends because group formation genuinely takes seconds and shows the
     * peer a system invitation prompt; the caller needs to be able to show
     * that as its own state and to cancel it.
     */
    @SuppressLint("MissingPermission")  // Guarded by unavailableReason below.
    suspend fun connectToOwner(
        context: Context,
        deviceAddress: String,
        timeoutMs: Long = 30_000,
    ): ConnectResult {
        val unavailable = unavailableReason(context)
        if (unavailable != null) return ConnectResult(null, unavailable)
        if (!ensureChannel(context)) {
            return ConnectResult(null, "the Wi-Fi Direct service did not start")
        }
        val mgr = manager ?: return ConnectResult(null, "no Wi-Fi Direct service")
        val ch = channel ?: return ConnectResult(null, "no Wi-Fi Direct channel")

        // A scan still running makes group formation flaky on several stacks,
        // and we have no use for one now.
        stopDiscovery()
        // Hosting is over the moment this device decides to join someone
        // else's game, and the group has to go with it - see above.
        stopAdvertising(context)
        if (!leaveAnyGroup(context, mgr, ch)) {
            return ConnectResult(
                null,
                "this device is still in a Wi-Fi Direct group of its own - turn Wi-Fi " +
                    "off and on again, or disconnect it under Settings, Wi-Fi, Wi-Fi Direct"
            )
        }

        return suspendCancellableCoroutine { continuation ->
            var settled = false

            fun finish(result: ConnectResult) {
                if (settled) return
                settled = true
                unregisterConnectReceiver(context)
                connectHandler.removeCallbacksAndMessages(null)
                if (continuation.isActive) continuation.resume(result)
            }

            val receiver = object : BroadcastReceiver() {
                override fun onReceive(ctx: Context, intent: Intent) {
                    if (intent.action != WifiP2pManager.WIFI_P2P_CONNECTION_CHANGED_ACTION) return
                    // Ask rather than reading the intent's extra: the extra is
                    // only populated on some versions, the request never isn't.
                    mgr.requestConnectionInfo(ch) { info ->
                        Log.i(
                            TAG,
                            "connection changed: formed=${info?.groupFormed} " +
                                "owner=${info?.isGroupOwner} address=${info?.groupOwnerAddress?.hostAddress}"
                        )
                        if (info != null && info.groupFormed) {
                            // Being the owner here means the negotiation went
                            // the wrong way: the host is the one with the
                            // game listening, and its address is not this
                            // one. Worth naming, because it is a different
                            // fault from no group at all.
                            if (info.isGroupOwner) {
                                finish(
                                    ConnectResult(
                                        null,
                                        "the group formed with this device as owner, so the " +
                                            "other device is not the one hosting"
                                    )
                                )
                            } else {
                                finish(ConnectResult(info.groupOwnerAddress?.hostAddress, null))
                            }
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
            mgr.connect(ch, config, actionListener("connect") { requested, reason ->
                if (!requested) finish(ConnectResult(null, connectFailure(reason)))
            })

            connectHandler.postDelayed({
                Log.e(TAG, "Wi-Fi Direct group did not form within ${timeoutMs}ms")
                finish(
                    ConnectResult(
                        null,
                        "the other device never answered - it may be showing an invitation " +
                            "prompt that needs accepting"
                    )
                )
            }, timeoutMs)

            continuation.invokeOnCancellation { finish(ConnectResult(null, null)) }
        }
    }

    /**
     * Leaves whatever group this device is in, and waits for it to actually
     * be gone rather than for the request to be accepted - `connect()` called
     * while the old group is still tearing down behaves as though it were
     * still a member. Answers false if the group outlives the wait.
     *
     * A no-op, and immediate, for the common case of a device that is in no
     * group. The case that matters is a group left over from a game this
     * device hosted earlier: it survives the game ending and the process
     * dying, so it is not enough to track hosting in a flag.
     */
    @SuppressLint("MissingPermission")  // Only called from guarded paths.
    private suspend fun leaveAnyGroup(
        context: Context,
        mgr: WifiP2pManager,
        ch: WifiP2pManager.Channel,
    ): Boolean = suspendCancellableCoroutine { continuation ->
        var settled = false
        var waited = 0L

        fun settle(left: Boolean) {
            if (settled) return
            settled = true
            if (continuation.isActive) continuation.resume(left)
        }

        fun poll() {
            if (unavailableReason(context) != null) return settle(false)
            mgr.requestGroupInfo(ch) { group ->
                if (group == null) return@requestGroupInfo settle(true)
                if (waited == 0L) {
                    Log.i(TAG, "leaving own group ${group.networkName} before connecting")
                    mgr.removeGroup(ch, actionListener("removeGroup"))
                }
                if (waited >= GROUP_TEARDOWN_TIMEOUT_MS) {
                    Log.e(TAG, "still in group ${group.networkName} after ${waited}ms")
                    return@requestGroupInfo settle(false)
                }
                waited += GROUP_TEARDOWN_POLL_MS
                connectHandler.postDelayed({ poll() }, GROUP_TEARDOWN_POLL_MS)
            }
        }
        poll()

        // The poll only continues from inside requestGroupInfo's callback, so
        // a channel that has quietly died would leave the caller suspended
        // for good. Nothing else in this file can strand a coroutine.
        connectHandler.postDelayed({
            if (!settled) Log.e(TAG, "requestGroupInfo never answered")
            settle(false)
        }, GROUP_TEARDOWN_TIMEOUT_MS + 1_000L)
    }

    /** The framework's connect() refusals, as something a player can read. */
    private fun connectFailure(reason: Int): String = when (reason) {
        WifiP2pManager.P2P_UNSUPPORTED -> "this device does not support Wi-Fi Direct"
        WifiP2pManager.BUSY -> "the Wi-Fi Direct service is busy - try again in a moment"
        WifiP2pManager.NO_SERVICE_REQUESTS -> "there was no service request outstanding"
        else -> "the connection request was refused (reason $reason)"
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

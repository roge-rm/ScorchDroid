package com.rm.scorchdroid

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothServerSocket
import android.bluetooth.BluetoothSocket
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.location.LocationManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.util.Log
import androidx.core.content.ContextCompat
import androidx.core.location.LocationManagerCompat
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.util.UUID
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.atomic.AtomicInteger

/**
 * Playing over Bluetooth: the last way in, and the only one that needs no
 * Wi-Fi at all, no router, no hotspot and no Play Services.
 *
 * This file is sockets and threads and nothing else. Everything the game
 * needs from a network - destination ids, connect and disconnect messages,
 * the hand-off to the engine thread - is in NetBridge on the C++ side, which
 * knows this only as a [BridgeTransport]. That split is what let the whole
 * path be proven on a build machine before any of this existed: host-tests
 * runs the real join handshake through NetBridge over a Unix socket pair.
 *
 * **Framing is this file's job.** A BridgeTransport promises whole messages,
 * and an RFCOMM stream promises nothing of the sort, so every message goes
 * out behind a four-byte big-endian length - the same shape
 * NetServerTCPProtocol puts on a TCP socket, for the same reason.
 *
 * The traffic fits with room to spare, which was measured rather than
 * assumed before any of this was written: steady play is about 1 kbit/s and
 * the whole of a join is one 46KB burst (the mod manifest), against RFCOMM's
 * low hundreds of kbit/s. See docs/p2p-transport-plan.md.
 */
object BluetoothTransport {
    private const val TAG = "BluetoothTransport"

    // The service both ends look for. Arbitrary but fixed: this is the whole
    // of how a ScorchDroid host is told apart from a headset.
    private val SERVICE_UUID: UUID = UUID.fromString("7f3a6c14-5b8e-4a2d-9c61-2e0d5f8b41a7")
    private const val SERVICE_NAME = "ScorchDroid"

    // Peers start at 2 to match what a NetServerTCP3 destination id looks
    // like; 0 and UINT_MAX mean something else to the engine (see
    // NetBridge::onPeerConnected, which rejects them).
    private val nextPeerId = AtomicInteger(2)

    private var appContext: Context? = null
    private val sockets = ConcurrentHashMap<Int, BluetoothSocket>()
    private val outputs = ConcurrentHashMap<Int, OutputStream>()
    private var serverSocket: BluetoothServerSocket? = null
    private var acceptThread: Thread? = null
    private val readerThreads = ConcurrentHashMap<Int, Thread>()
    @Volatile private var stopping = false

    private var discoveryReceiver: BroadcastReceiver? = null
    private var discoveryTimeout: Runnable? = null
    private val mainHandler = Handler(Looper.getMainLooper())

    /** Called once at startup: everything below needs a context and has none. */
    fun init(context: Context) {
        appContext = context.applicationContext
    }

    private fun adapter(): BluetoothAdapter? {
        val context = appContext ?: return null
        val manager = context.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager
        return manager?.adapter
    }

    /**
     * The permissions Bluetooth needs at runtime, which were rearranged in
     * API 31. Before that, scanning was treated as a location capability;
     * from 31 there are three dedicated permissions and BLUETOOTH_SCAN can
     * be declared neverForLocation, so the game never asks for location on a
     * modern device. The same shape as [WifiDirectTransport.requiredPermissions].
     */
    fun requiredPermissions(): Array<String> =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            arrayOf(
                Manifest.permission.BLUETOOTH_CONNECT,
                Manifest.permission.BLUETOOTH_SCAN,
                Manifest.permission.BLUETOOTH_ADVERTISE,
            )
        } else {
            arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)
        }

    fun hasPermissions(context: Context): Boolean =
        requiredPermissions().all {
            ContextCompat.checkSelfPermission(context, it) == PackageManager.PERMISSION_GRANTED
        }

    fun isSupported(context: Context): Boolean {
        if (!context.packageManager.hasSystemFeature(PackageManager.FEATURE_BLUETOOTH)) return false
        val manager = context.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager
        return manager?.adapter != null
    }

    /**
     * Why Bluetooth cannot be used right now, in words a player can act on,
     * or null if it can. The Wi-Fi Direct work is the reason this exists at
     * all: every silent early return there turned a two-device test into a
     * guessing game.
     */
    fun unavailableReason(context: Context): String? {
        if (!isSupported(context)) return "this device has no Bluetooth"
        if (!hasPermissions(context)) {
            return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                "the Bluetooth permissions were refused"
            } else {
                "the location permission was refused - Android ${Build.VERSION.RELEASE} " +
                    "treats finding nearby Bluetooth devices as a location feature"
            }
        }
        if (adapter()?.isEnabled != true) return "Bluetooth is switched off"
        // Below API 31 this is the same trap Wi-Fi Direct has: the permission
        // is not enough, and with the master location toggle off a scan
        // starts, succeeds, and reports nothing, forever. Already-paired
        // devices still list, so the failure looks like "only pairing works"
        // rather than like a switch being off. From 31 the dedicated
        // BLUETOOTH_SCAN permission covers it and the toggle is irrelevant.
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S && !isLocationEnabled(context)) {
            return "Location is switched off in system settings, which Android " +
                "${Build.VERSION.RELEASE} needs before it will find unpaired devices"
        }
        return null
    }

    private fun isLocationEnabled(context: Context): Boolean {
        val lm = context.applicationContext.getSystemService(Context.LOCATION_SERVICE)
            as? LocationManager ?: return false
        return LocationManagerCompat.isLocationEnabled(lm)
    }

    /** The device's own Bluetooth name, for the host to show as its label. */
    @SuppressLint("MissingPermission")  // Guarded by unavailableReason.
    fun localName(context: Context): String {
        if (unavailableReason(context) != null) return Build.MODEL
        return adapter()?.name ?: Build.MODEL
    }

    /**
     * Whether other devices can currently *find* this one, as opposed to
     * merely connect to it once they know it.
     *
     * Worth asking rather than assuming, because the two are different states
     * and only one of them is what an unpaired player needs: a phone that is
     * connectable but not discoverable is invisible to a scan while remaining
     * perfectly joinable by anything already paired with it - which is
     * exactly the shape of "only pairing works".
     */
    @SuppressLint("MissingPermission")  // Guarded by unavailableReason.
    fun isDiscoverable(context: Context): Boolean {
        if (unavailableReason(context) != null) return false
        return adapter()?.scanMode == BluetoothAdapter.SCAN_MODE_CONNECTABLE_DISCOVERABLE
    }

    /** Whether the radio is present but switched off - the one fixable case. */
    fun isOff(context: Context): Boolean = isSupported(context) && adapter()?.isEnabled != true

    /**
     * The system's own "turn Bluetooth on?" prompt. Offered rather than
     * merely reported: a player who has chosen to host over Bluetooth has
     * said what they want, and the alternative is telling them to go and
     * find a settings screen.
     */
    fun enableIntent(): Intent = Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE)

    /**
     * The system dialog that makes this device findable by a phone that has
     * never paired with it. Without it, only already-paired devices can see
     * a host - so it is asked for when hosting begins rather than left for
     * the player to find in Settings.
     */
    fun discoverableIntent(): Intent =
        Intent(BluetoothAdapter.ACTION_REQUEST_DISCOVERABLE).apply {
            putExtra(BluetoothAdapter.EXTRA_DISCOVERABLE_DURATION, DISCOVERABLE_SECONDS)
        }

    // Long enough to set a game up without the host having to re-ask, and
    // the longest Android will accept without the value being ignored.
    private const val DISCOVERABLE_SECONDS = 300

    /**
     * Looks for nearby devices for [durationMs] and reports each one, as
     * [LanDiscovery.FoundGame] carrying a `bluetoothAddress`, so the "Find
     * Games" dialog can merge them with everything else it found.
     *
     * Both already-paired devices and a live scan, and deliberately without
     * asking whether each one actually serves our UUID: an SDP lookup per
     * device is slow, frequently answers nothing on a device that is in fact
     * running the game, and would leave the player staring at an empty list
     * for the one transport that is meant to be the fallback. A device that
     * turns out not to be a host fails the connect in a second or two and
     * says so.
     */
    @SuppressLint("MissingPermission")  // Guarded by unavailableReason.
    fun startDiscovery(
        context: Context,
        durationMs: Long,
        onFound: (LanDiscovery.FoundGame) -> Unit,
        onFinished: () -> Unit,
    ) {
        val reason = unavailableReason(context)
        if (reason != null) {
            Log.w(TAG, "not scanning: $reason")
            onFinished()
            return
        }
        val adapter = adapter() ?: return onFinished()
        stopDiscovery(context)

        val seen = mutableSetOf<String>()
        fun report(device: BluetoothDevice, paired: Boolean) {
            val address = device.address ?: return
            if (!seen.add(address)) return
            val name = device.name ?: address
            Log.i(TAG, "device: $name $address paired=$paired")
            onFound(
                LanDiscovery.FoundGame(
                    name = name,
                    host = "",
                    port = 0,
                    bluetoothAddress = address,
                    bluetoothPaired = paired,
                )
            )
        }

        // Paired devices first and without waiting: the likely case is two
        // phones that have played before, and they are known instantly while
        // a scan takes ten seconds or more.
        adapter.bondedDevices?.forEach { report(it, true) }

        // Counted so the end of the scan can say whether the radio found
        // nothing or was never really looking. Those are different faults and
        // from the outside they are the same empty list.
        var inquiryStarted = false
        var devicesSeen = 0
        // Whether the adapter ever admitted to scanning, polled below.
        var sawDiscovering = false

        val receiver = object : BroadcastReceiver() {
            override fun onReceive(ctx: Context, intent: Intent) {
                // The two that prove a scan actually ran. Without them, a
                // scan that the framework declined to start and a scan that
                // ran and found nobody are indistinguishable.
                if (intent.action == BluetoothAdapter.ACTION_DISCOVERY_STARTED) {
                    inquiryStarted = true
                    Log.i(TAG, "inquiry started")
                    return
                }
                if (intent.action == BluetoothAdapter.ACTION_DISCOVERY_FINISHED) {
                    Log.i(TAG, "inquiry finished after $devicesSeen device(s)")
                    return
                }
                if (intent.action != BluetoothDevice.ACTION_FOUND) return
                devicesSeen++
                val device = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                    intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE, BluetoothDevice::class.java)
                } else {
                    @Suppress("DEPRECATION")
                    intent.getParcelableExtra<BluetoothDevice>(BluetoothDevice.EXTRA_DEVICE)
                } ?: return
                mainHandler.post { report(device, false) }
            }
        }
        discoveryReceiver = receiver
        // RECEIVER_EXPORTED, where every other receiver in this port is
        // NOT_EXPORTED, and it is not a relaxation of anything.
        //
        // These three come from the Bluetooth module, which since Android 12
        // is a separate APEX app with its own UID - not the system server
        // that sends Wi-Fi P2P's. NOT_EXPORTED means "same app only", so it
        // dropped every one of them: the radio ran a full twelve-second
        // inquiry, found the other phone, and this app was told nothing.
        // From the outside that is indistinguishable from there being no
        // other phone, which is how it read for two evenings.
        //
        // Exporting costs nothing here because all three are *protected*
        // broadcasts: the platform refuses to let any app but the system
        // send them, so there is no one else who could deliver one.
        ContextCompat.registerReceiver(
            context.applicationContext,
            receiver,
            IntentFilter().apply {
                addAction(BluetoothDevice.ACTION_FOUND)
                addAction(BluetoothAdapter.ACTION_DISCOVERY_STARTED)
                addAction(BluetoothAdapter.ACTION_DISCOVERY_FINISHED)
            },
            ContextCompat.RECEIVER_EXPORTED,
        )

        if (adapter.isDiscovering) adapter.cancelDiscovery()
        if (!adapter.startDiscovery()) {
            Log.e(TAG, "startDiscovery was refused")
            stopDiscovery(context)
            onFinished()
            return
        }
        Log.i(TAG, "scanning for ${durationMs}ms, ${adapter.bondedDevices?.size ?: 0} device(s) already paired")

        // Ask the adapter what it is doing, rather than only waiting to be
        // told. A scan that the radio is genuinely running and a scan whose
        // broadcasts are not reaching this app look identical from the
        // receiver's side - and they are opposite problems, one in the
        // framework's willingness to look and one in this file's own
        // registration.
        val discoveringPoll = object : Runnable {
            var ticks = 0
            override fun run() {
                if (discoveryTimeout == null) return
                val discovering = try {
                    adapter.isDiscovering
                } catch (e: SecurityException) {
                    false
                }
                if (discovering) sawDiscovering = true
                if (ticks < 4 || discovering) {
                    Log.i(TAG, "adapter.isDiscovering = $discovering (${ticks}s in)")
                }
                ticks++
                mainHandler.postDelayed(this, 1000L)
            }
        }
        mainHandler.postDelayed(discoveringPoll, 1000L)

        discoveryTimeout = Runnable {
            discoveryTimeout = null
            // The whole point of the two counters: a scan the framework never
            // actually ran is a different problem from a scan that ran and
            // saw nobody, and only one of them is about the other phone.
            mainHandler.removeCallbacks(discoveringPoll)
            if (!inquiryStarted && sawDiscovering) {
                // The radio looked. We simply never heard it say so, which
                // makes this a fault in how these broadcasts are registered
                // for and nothing to do with the other phone.
                Log.e(TAG, "the radio scanned but no broadcast reached us - receiver registration")
            } else if (!inquiryStarted) {
                Log.e(TAG, "the scan never started - startDiscovery said yes and the adapter never scanned")
            } else if (devicesSeen == 0) {
                Log.w(TAG, "the scan ran and saw no unpaired device at all")
            }
            stopDiscovery(context)
            onFinished()
        }
        mainHandler.postDelayed(discoveryTimeout!!, durationMs)
    }

    @SuppressLint("MissingPermission")
    fun stopDiscovery(context: Context) {
        discoveryTimeout?.let { mainHandler.removeCallbacks(it) }
        discoveryTimeout = null
        discoveryReceiver?.let { receiver ->
            discoveryReceiver = null
            try {
                context.applicationContext.unregisterReceiver(receiver)
            } catch (e: IllegalArgumentException) {
                // Never registered, or already gone.
            }
        }
        if (hasPermissions(context)) {
            try {
                adapter()?.takeIf { it.isDiscovering }?.cancelDiscovery()
            } catch (e: SecurityException) {
                // Permission revoked mid-scan; the scan is over either way.
            }
        }
    }

    // ---------------------------------------------------------------------
    // The BridgeTransport half: called from C++ (JniTransport.cpp), and
    // answering on the reader threads started here. Everything below this
    // line runs off the main thread.
    // ---------------------------------------------------------------------

    private external fun nativePeerConnected(peerId: Int)
    private external fun nativePayload(peerId: Int, bytes: ByteArray, length: Int)
    private external fun nativePeerDisconnected(peerId: Int)
    private external fun nativeFailed(reason: String)

    @JvmStatic
    @SuppressLint("MissingPermission")  // Checked below.
    fun nativeStartListening(): Boolean {
        val context = appContext ?: return false
        val reason = unavailableReason(context)
        if (reason != null) {
            Log.e(TAG, "cannot host: $reason")
            return false
        }
        val adapter = adapter() ?: return false

        stopping = false
        return try {
            val socket = adapter.listenUsingRfcommWithServiceRecord(SERVICE_NAME, SERVICE_UUID)
            serverSocket = socket
            acceptThread = Thread({ acceptLoop(socket) }, "ScorchDroid-bt-accept").also { it.start() }
            Log.i(TAG, "listening for players over Bluetooth")
            true
        } catch (e: IOException) {
            Log.e(TAG, "could not open a Bluetooth service socket", e)
            false
        } catch (e: SecurityException) {
            Log.e(TAG, "not allowed to open a Bluetooth service socket", e)
            false
        }
    }

    @JvmStatic
    @SuppressLint("MissingPermission")  // Checked below.
    fun nativeConnectTo(address: String): Boolean {
        val context = appContext ?: return false
        val reason = unavailableReason(context)
        if (reason != null) {
            Log.e(TAG, "cannot join: $reason")
            return false
        }
        val adapter = adapter() ?: return false

        stopping = false
        // Connecting is a blocking call that takes seconds, and a bridge
        // transport reports success by calling back rather than by returning
        // it - which is what lets this happen off the caller's thread.
        Thread({
            try {
                // Mandatory, not tidiness: a discovery in progress slows the
                // radio to the point where connects routinely fail.
                if (adapter.isDiscovering) adapter.cancelDiscovery()

                val device = adapter.getRemoteDevice(address)
                val socket = device.createRfcommSocketToServiceRecord(SERVICE_UUID)
                socket.connect()
                addPeer(socket)
                Log.i(TAG, "connected to $address")
            } catch (e: IOException) {
                Log.e(TAG, "could not reach $address", e)
                nativeFailed(
                    "couldn't open a Bluetooth connection - check the other device is " +
                        "hosting, and pair the two if they never have been"
                )
            } catch (e: SecurityException) {
                Log.e(TAG, "not allowed to connect to $address", e)
                nativeFailed("the Bluetooth permission was refused")
            }
        }, "ScorchDroid-bt-connect").start()
        return true
    }

    @JvmStatic
    fun nativeSend(peerId: Int, bytes: ByteArray): Boolean {
        val output = outputs[peerId] ?: return false
        return try {
            // One synchronized write per message: the engine sends from a
            // single thread today, but a half-written length prefix is
            // unrecoverable and this costs nothing to rule out.
            synchronized(output) {
                val length = bytes.size
                output.write(
                    byteArrayOf(
                        ((length ushr 24) and 0xff).toByte(),
                        ((length ushr 16) and 0xff).toByte(),
                        ((length ushr 8) and 0xff).toByte(),
                        (length and 0xff).toByte(),
                    )
                )
                output.write(bytes)
                output.flush()
            }
            true
        } catch (e: IOException) {
            Log.e(TAG, "send to peer $peerId failed", e)
            false
        }
    }

    @JvmStatic
    fun nativeDisconnect(peerId: Int) {
        closePeer(peerId)
    }

    @JvmStatic
    fun nativeStop() {
        stopping = true
        try {
            serverSocket?.close()
        } catch (e: IOException) {
            // Already closed; the accept thread is on its way out either way.
        }
        serverSocket = null
        sockets.keys.toList().forEach { closePeer(it) }
        acceptThread = null
        readerThreads.clear()
        Log.i(TAG, "stopped")
    }

    @SuppressLint("MissingPermission")
    private fun acceptLoop(socket: BluetoothServerSocket) {
        while (!stopping) {
            val accepted = try {
                socket.accept()
            } catch (e: IOException) {
                // The expected way out: nativeStop closed the socket under us.
                if (!stopping) Log.e(TAG, "accept failed", e)
                break
            }
            if (accepted != null) addPeer(accepted)
        }
    }

    private fun addPeer(socket: BluetoothSocket) {
        val peerId = nextPeerId.getAndIncrement()
        sockets[peerId] = socket
        try {
            outputs[peerId] = socket.outputStream
        } catch (e: IOException) {
            Log.e(TAG, "peer $peerId had no output stream", e)
            closePeer(peerId)
            return
        }

        val reader = Thread({ readLoop(peerId, socket) }, "ScorchDroid-bt-read-$peerId")
        readerThreads[peerId] = reader
        // Announced before the reader starts, so the engine knows the
        // destination before the first message from it can arrive.
        nativePeerConnected(peerId)
        reader.start()
    }

    private fun readLoop(peerId: Int, socket: BluetoothSocket) {
        val header = ByteArray(4)
        try {
            val input = socket.inputStream
            while (!stopping) {
                if (!readFully(input, header, 4)) break

                val length =
                    ((header[0].toInt() and 0xff) shl 24) or
                        ((header[1].toInt() and 0xff) shl 16) or
                        ((header[2].toInt() and 0xff) shl 8) or
                        (header[3].toInt() and 0xff)
                // The same bound the TCP protocol applies, for the same
                // reason: a length that is wrong is wrong by a lot, and
                // allocating on it is how a corrupt stream becomes a crash.
                if (length <= 0 || length > MAX_MESSAGE_BYTES) {
                    Log.e(TAG, "peer $peerId sent an impossible length $length")
                    break
                }

                val payload = ByteArray(length)
                if (!readFully(input, payload, length)) break
                nativePayload(peerId, payload, length)
            }
        } catch (e: IOException) {
            if (!stopping) Log.e(TAG, "read from peer $peerId failed", e)
        }

        closePeer(peerId)
    }

    private fun readFully(input: InputStream, into: ByteArray, length: Int): Boolean {
        var got = 0
        while (got < length) {
            val read = input.read(into, got, length - got)
            if (read <= 0) return false
            got += read
        }
        return true
    }

    /**
     * Closes one peer and tells the engine once. Both ends of a dropped link
     * and a deliberate kick arrive here, and the engine must not hear about
     * the same player leaving twice - it counts that as two.
     */
    private fun closePeer(peerId: Int) {
        val socket = sockets.remove(peerId) ?: return
        outputs.remove(peerId)
        readerThreads.remove(peerId)
        try {
            socket.close()
        } catch (e: IOException) {
            // Already closed.
        }
        nativePeerDisconnected(peerId)
    }

    private const val MAX_MESSAGE_BYTES = 5_000_000
}

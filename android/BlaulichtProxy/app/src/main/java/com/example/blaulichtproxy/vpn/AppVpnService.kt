package com.example.blaulichtproxy.vpn

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.content.Intent
import android.content.pm.ServiceInfo
import android.net.VpnService
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.ParcelFileDescriptor
import android.util.Log
import android.net.TrafficStats
import androidx.core.app.NotificationCompat

import com.example.blaulichtproxy.R
import com.example.blaulichtproxy.tun.Tun2SocksBridge
import mobile.TunnelHandle
import java.io.File
import java.io.IOException
import java.net.InetSocketAddress
import java.net.Socket

class AppVpnService : VpnService() {

    companion object {
        const val EXTRA_PROXY_HOST = "proxy_host"
        const val EXTRA_PROXY_PORT = "proxy_port"
        const val EXTRA_TARGET_PKG = "target_pkg"

        const val ACTION_START = "com.example.blaulichtproxy.action.START_VPN"
        const val ACTION_STOP  = "com.example.blaulichtproxy.action.STOP_VPN"

        // Broadcast updates so UI can reflect state
        const val ACTION_STATE = "com.example.blaulichtproxy.action.VPN_STATE"
        const val EXTRA_STATE  = "state"
        const val EXTRA_MESSAGE = "message"

    // Periodic metrics broadcast (traffic + proxy reachability)
    const val ACTION_METRICS = "com.example.blaulichtproxy.action.METRICS"
    const val EXTRA_RX_BPS = "rx_bps"
    const val EXTRA_TX_BPS = "tx_bps"
    const val EXTRA_RX_TOTAL = "rx_total"
    const val EXTRA_TX_TOTAL = "tx_total"
    const val EXTRA_PROXY_ONLINE = "proxy_online"

        // States
        const val STATE_INACTIVE = "INACTIVE"
        const val STATE_CONNECTING = "CONNECTING"
        const val STATE_CONNECTED = "CONNECTED"
        const val STATE_ERROR = "ERROR"

        private const val CH_ID = "vpn_channel"
        private const val N_ID = 42
        private const val TAG = "AppVpnService"
    }

    private var pfd: ParcelFileDescriptor? = null
    @Volatile private var tunnel: TunnelHandle? = null
    @Volatile private var isRunning = false
    private val mainHandler = Handler(Looper.getMainLooper())
    private var metricsRunnable: Runnable? = null
    private var lastRx: Long = 0L
    private var lastTx: Long = 0L
    private var lastSampleTs: Long = 0L
    private var currentHost: String = ""
    private var currentPort: Int = 0

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_STOP -> {
                Log.d(TAG, "onStartCommand: ACTION_STOP")
                // Mark as not running so notifications aren't ongoing
                isRunning = false

                // Clean up everything immediately
                runCatching { Tun2SocksBridge.stop(tunnel) }
                tunnel = null

                runCatching { pfd?.close() }
                pfd = null

                // Remove our foreground notification
                stopForeground(STOP_FOREGROUND_REMOVE)

                sendState(STATE_INACTIVE, "Stopped")
                // Stop the service (will also tear down the VPN)
                stopSelf()
                return START_NOT_STICKY
            }
        }

        // --- existing START logic below here (unchanged) ---
    val host = intent?.getStringExtra(EXTRA_PROXY_HOST) ?: return START_NOT_STICKY
    val port = intent.getIntExtra(EXTRA_PROXY_PORT, 8281)
        val pkg  = intent.getStringExtra(EXTRA_TARGET_PKG).orEmpty()

    Log.d(TAG, "onStartCommand: ACTION_START host=$host port=$port targetPkg=$pkg")

        if (isRunning) {
            Log.d(TAG, "VPN already running, ignoring duplicate start")
            sendState(STATE_CONNECTED, "Already running")
            return START_STICKY
        }
    isRunning = true
    currentHost = host
    currentPort = port
        sendState(STATE_CONNECTING, "Connecting $pkg via $host:$port")

        // "Connecting..." foreground notification
        showNotification("Connecting $pkg via $host:$port")

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            startForeground(
                N_ID,
                buildNotification("Connecting $pkg via $host:$port"),
                ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC
            )
        } else {
            startForeground(N_ID, buildNotification("Connecting $pkg via $host:$port"))
        }

        dumpEnv()

        // Build the TUN interface
        val builder = Builder()
            .setSession("Per-app VPN")
            .setMtu(1500)
            .addAddress("10.0.0.1", 32)
            .addDnsServer("1.1.1.1")

        if (pkg.isNotEmpty()) {
            runCatching { builder.addAllowedApplication(pkg) }
                .onFailure { Log.e(TAG, "addAllowedApplication($pkg) failed", it) }
        }

        // Exclude our own app so it can reach your PC directly
        runCatching { builder.addDisallowedApplication(packageName) }

        builder.addRoute("0.0.0.0", 0)
        builder.addRoute("::", 0)

        pfd = builder.establish()
        if (pfd == null) {
            Log.e(TAG, "Failed to establish VPN")
            stopSelf()
            return START_NOT_STICKY
        }
        val fd = pfd!!.detachFd()
        Log.d(TAG, "VPN established, fd=$fd — starting tun2socks thread")

        Thread {
            if (!canReachProxy(host, port)) {
                Log.e(TAG, "Preflight: cannot reach $host:$port, stopping.")
                showNotification("Proxy $host:$port unreachable")
                sendState(STATE_ERROR, "Proxy unreachable")
                stopSelf()
                return@Thread
            }

            try {
                Log.d(TAG, "Tun2Socks: calling start(fd=$fd, $host:$port)")
                // ✅ FIX: no udp argument here
                val h = Tun2SocksBridge.start(fd, host, port)
                tunnel = h
                Log.d(TAG, "Tun2Socks: started OK")
                showNotification("Connected $pkg via $host:$port")
                sendState(STATE_CONNECTED, "Connected $pkg")
                startMetricsLoop()
            } catch (t: Throwable) {
                Log.e(TAG, "Tun2Socks: FAILED", t)
                showNotification("Error starting VPN: ${t.message}")
                sendState(STATE_ERROR, t.message ?: "Unknown error")
                stopSelf()
            }
        }.start()

        // Watchdog: if still not started after 10s, stop so it doesn’t “load forever”
        Handler(Looper.getMainLooper()).postDelayed({
            if (tunnel == null && isRunning) {
                Log.e(TAG, "WATCHDOG: tun2socks did not start in 10s → stopping VPN")
                showNotification("VPN failed to start")
                sendState(STATE_ERROR, "Startup timeout")
                stopSelf()
            }
        }, 10_000)

        return START_STICKY
    }

    override fun onDestroy() {
        Log.d(TAG, "onDestroy: stopping tunnel + closing fd")
        isRunning = false
        stopMetricsLoop()
        runCatching { Tun2SocksBridge.stop(tunnel) }
        tunnel = null
        runCatching { pfd?.close() }
        pfd = null
        stopForeground(STOP_FOREGROUND_REMOVE)
        sendState(STATE_INACTIVE, "Destroyed")
        super.onDestroy()
    }

    private fun canReachProxy(host: String, port: Int, timeoutMs: Int = 3000): Boolean {
        Log.d(TAG, "Preflight: try TCP $host:$port (timeout=${timeoutMs}ms)")
        return try {
            Socket().use { s ->
                s.connect(InetSocketAddress(host, port), timeoutMs)
            }
            Log.d(TAG, "Preflight: SUCCESS")
            true
        } catch (e: IOException) {
            Log.e(TAG, "Preflight: FAILED to $host:$port", e)
            false
        }
    }

    private fun dumpEnv() {
        val abi = Build.SUPPORTED_ABIS?.joinToString()
        val libDir = applicationInfo.nativeLibraryDir
        Log.d(TAG, "ABI: $abi")
        Log.d(TAG, "nativeLibraryDir: $libDir")
        try {
            File(libDir).list()?.forEach {
                Log.d(TAG, "native lib: $it")
            }
        } catch (_: Throwable) {
        }
    }

    // --- Notification helpers ---

    private fun buildNotification(text: String): Notification {
        if (Build.VERSION.SDK_INT >= 26) {
            val nm = getSystemService(NotificationManager::class.java)
            if (nm.getNotificationChannel(CH_ID) == null) {
                nm.createNotificationChannel(
                    NotificationChannel(CH_ID, "VPN", NotificationManager.IMPORTANCE_LOW)
                )
            }
        }

        // Use a neutral static icon so it doesn’t look like a perpetual download.
        return NotificationCompat.Builder(this, CH_ID)
            .setSmallIcon(android.R.drawable.ic_lock_lock)
            .setContentTitle("App VPN")
            .setContentText(text)
            .setOngoing(isRunning)
            .build()
    }

    private fun showNotification(text: String) {
        val nm = getSystemService(NotificationManager::class.java)
        nm.notify(N_ID, buildNotification(text))
    }

    private fun sendState(state: String, message: String? = null) {
        val i = Intent(ACTION_STATE).apply {
            putExtra(EXTRA_STATE, state)
            message?.let { putExtra(EXTRA_MESSAGE, it) }
        }
        sendBroadcast(i)
    }

    // --- Metrics loop ---

    private fun startMetricsLoop() {
        if (metricsRunnable != null) return
        lastRx = safeUidRxBytes()
        lastTx = safeUidTxBytes()
        lastSampleTs = System.currentTimeMillis()

        metricsRunnable = Runnable {
            if (!isRunning) return@Runnable
            val now = System.currentTimeMillis()
            val dtMs = (now - lastSampleTs).coerceAtLeast(1)
            val rx = safeUidRxBytes()
            val tx = safeUidTxBytes()
            var drx = rx - lastRx
            var dtx = tx - lastTx
            if (drx < 0) drx = 0
            if (dtx < 0) dtx = 0
            val rxBps = (drx * 1000L) / dtMs
            val txBps = (dtx * 1000L) / dtMs

            lastRx = rx
            lastTx = tx
            lastSampleTs = now

            // probe proxy reachability occasionally to reflect health
            val proxyOk = canReachProxy(currentHost, currentPort, timeoutMs = 800)
            sendMetrics(rxBps, txBps, rx, tx, proxyOk)

            // schedule next sample
            mainHandler.postDelayed(metricsRunnable!!, 1000)
        }
        mainHandler.postDelayed(metricsRunnable!!, 1000)
    }

    private fun stopMetricsLoop() {
        metricsRunnable?.let { mainHandler.removeCallbacks(it) }
        metricsRunnable = null
    }

    private fun sendMetrics(rxBps: Long, txBps: Long, rxTotal: Long, txTotal: Long, proxyOnline: Boolean) {
        val i = Intent(ACTION_METRICS).apply {
            putExtra(EXTRA_RX_BPS, rxBps)
            putExtra(EXTRA_TX_BPS, txBps)
            putExtra(EXTRA_RX_TOTAL, rxTotal)
            putExtra(EXTRA_TX_TOTAL, txTotal)
            putExtra(EXTRA_PROXY_ONLINE, proxyOnline)
        }
        sendBroadcast(i)
    }

    private fun safeUidRxBytes(): Long {
        val v = TrafficStats.getUidRxBytes(android.os.Process.myUid())
        return if (v == TrafficStats.UNSUPPORTED.toLong()) 0L else v
    }

    private fun safeUidTxBytes(): Long {
        val v = TrafficStats.getUidTxBytes(android.os.Process.myUid())
        return if (v == TrafficStats.UNSUPPORTED.toLong()) 0L else v
    }
}

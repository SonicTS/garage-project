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

        private const val CH_ID = "vpn_channel"
        private const val N_ID = 42
        private const val TAG = "AppVpnService"
    }

    private var pfd: ParcelFileDescriptor? = null
    @Volatile private var tunnel: TunnelHandle? = null
    @Volatile private var isRunning = false

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

                // Stop the service (will also tear down the VPN)
                stopSelf()
                return START_NOT_STICKY
            }
        }

        // --- existing START logic below here (unchanged) ---
        val host = intent?.getStringExtra(EXTRA_PROXY_HOST) ?: return START_NOT_STICKY
        val port = intent.getIntExtra(EXTRA_PROXY_PORT, 1080)
        val pkg  = intent.getStringExtra(EXTRA_TARGET_PKG).orEmpty()

        Log.d(TAG, "onStartCommand: ACTION_START host=$host port=$port targetPkg=$pkg")

        if (isRunning) {
            Log.d(TAG, "VPN already running, ignoring duplicate start")
            return START_STICKY
        }
        isRunning = true

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
            } catch (t: Throwable) {
                Log.e(TAG, "Tun2Socks: FAILED", t)
                showNotification("Error starting VPN: ${t.message}")
                stopSelf()
            }
        }.start()

        // Watchdog: if still not started after 10s, stop so it doesn’t “load forever”
        Handler(Looper.getMainLooper()).postDelayed({
            if (tunnel == null && isRunning) {
                Log.e(TAG, "WATCHDOG: tun2socks did not start in 10s → stopping VPN")
                showNotification("VPN failed to start")
                stopSelf()
            }
        }, 10_000)

        return START_STICKY
    }

    override fun onDestroy() {
        Log.d(TAG, "onDestroy: stopping tunnel + closing fd")
        isRunning = false
        runCatching { Tun2SocksBridge.stop(tunnel) }
        tunnel = null
        runCatching { pfd?.close() }
        pfd = null
        stopForeground(STOP_FOREGROUND_REMOVE)
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
}

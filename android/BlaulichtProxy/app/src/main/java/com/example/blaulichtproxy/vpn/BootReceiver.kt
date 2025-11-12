package com.example.blaulichtproxy.vpn

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.net.VpnService
import android.os.Build
import android.util.Log

/**
 * BootReceiver checks at device boot whether the user opted in to auto-starting
 * the per-app VPN. It NEVER bypasses Android's required user consent: if
 * VpnService.prepare(context) is non-null we won't start automatically.
 *
 * To enable autostart from your UI, set SharedPreferences key:
 *   prefs.edit().putBoolean("autostart_enabled", true).apply()
 * and optionally store proxy_host / proxy_port / target_pkg.
 */
class BootReceiver : BroadcastReceiver() {

    companion object {
        private const val TAG = "BootReceiver"
        private const val PREFS = "vpn_prefs"
        private const val KEY_ENABLED = "autostart_enabled"
        private const val KEY_HOST = "proxy_host"
        private const val KEY_PORT = "proxy_port"
        private const val KEY_PKG = "target_pkg"
    }

    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action != Intent.ACTION_BOOT_COMPLETED) return
        val prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

        if (!prefs.getBoolean(KEY_ENABLED, false)) {
            Log.d(TAG, "Autostart disabled; not starting VPN")
            return
        }

        // Ensure user previously granted VPN consent; can't auto-start otherwise.
        if (VpnService.prepare(context) != null) {
            Log.w(TAG, "VPN consent not granted yet; skipping autostart")
            return
        }

        val host = prefs.getString(KEY_HOST, "<YOUR_PROXY_HOST>") ?: "<YOUR_PROXY_HOST>"
        val port = prefs.getInt(KEY_PORT, 8281)
        val pkg  = prefs.getString(KEY_PKG, "") ?: ""

        Log.i(TAG, "Boot: starting AppVpnService host=$host port=$port pkg=$pkg")
        val startIntent = Intent(context, AppVpnService::class.java).apply {
            action = AppVpnService.ACTION_START
            putExtra(AppVpnService.EXTRA_PROXY_HOST, host)
            putExtra(AppVpnService.EXTRA_PROXY_PORT, port)
            putExtra(AppVpnService.EXTRA_TARGET_PKG, pkg)
        }
        if (Build.VERSION.SDK_INT >= 26) {
            context.startForegroundService(startIntent)
        } else {
            context.startService(startIntent)
        }
    }
}

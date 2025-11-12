package com.example.blaulichtproxy

import android.app.Activity
import android.content.Intent
import android.net.VpnService
import android.content.BroadcastReceiver
import android.content.Context
import android.content.IntentFilter
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.example.blaulichtproxy.vpn.AppVpnService
import android.text.format.Formatter
import android.content.SharedPreferences

class MainActivity : ComponentActivity() {

    // UI state for fields
    private var proxyHost by mutableStateOf("minecraftwgwg.hopto.org")          // later: your DDNS name
    private var proxyPort by mutableStateOf("8281")
    private var targetPkg by mutableStateOf("net.ut11.ccmp.blaulicht")
    private var autostart by mutableStateOf(false)
    private var vpnState by mutableStateOf("INACTIVE")
    private var isStarting by mutableStateOf(false)
    private lateinit var prefs: SharedPreferences
    private var rxBps by mutableStateOf(0L)
    private var txBps by mutableStateOf(0L)
    private var rxTotal by mutableStateOf(0L)
    private var txTotal by mutableStateOf(0L)
    private var proxyOnline by mutableStateOf(false)

    // VPN permission launcher
    private val vpnPrep = registerForActivityResult(
        ActivityResultContracts.StartActivityForResult()
    ) { res ->
        if (res.resultCode == Activity.RESULT_OK) {
            startVpnService()
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        prefs = getSharedPreferences("vpn_prefs", MODE_PRIVATE)
        // Restore last-used values
        proxyHost = prefs.getString("proxy_host", proxyHost) ?: proxyHost
        proxyPort = (prefs.getInt("proxy_port", proxyPort.toIntOrNull() ?: 8281)).toString()
        targetPkg = prefs.getString("target_pkg", targetPkg) ?: targetPkg
        autostart = prefs.getBoolean("autostart_enabled", false)
        val f = IntentFilter().apply {
            addAction(AppVpnService.ACTION_STATE)
            addAction(AppVpnService.ACTION_METRICS)
        }
        registerReceiver(stateReceiver, f)
        setContent { Ui() }
    }

    override fun onDestroy() {
        unregisterReceiver(stateReceiver)
        super.onDestroy()
    }

    @Composable
    private fun Ui() {
        MaterialTheme {
            Column(
                modifier = Modifier.padding(16.dp),
                verticalArrangement = Arrangement.spacedBy(12.dp)
            ) {
                OutlinedTextField(
                    value = proxyHost,
                    onValueChange = { proxyHost = it },
                    label = { Text("Proxy host (PC IP / DDNS)") },
                    singleLine = true
                )
                OutlinedTextField(
                    value = proxyPort,
                    onValueChange = { proxyPort = it },
                    label = { Text("Proxy port") },
                    singleLine = true
                )
                OutlinedTextField(
                    value = targetPkg,
                    onValueChange = { targetPkg = it },
                    label = { Text("Target package") },
                    singleLine = true
                )
                Row(
                    horizontalArrangement = Arrangement.spacedBy(12.dp),
                    modifier = Modifier.fillMaxWidth()
                ) {
                    Text("Start on boot")
                    Switch(checked = autostart, onCheckedChange = {
                        autostart = it
                        savePrefs()
                    })
                }
                Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                    val canStart = vpnState == "INACTIVE" && !isStarting
                    val canStop = vpnState == "CONNECTING" || vpnState == "CONNECTED" || isStarting
                    Button(onClick = { ensureVpnPermissionThenStart() }, enabled = canStart) {
                        Text("Start VPN")
                    }
                    OutlinedButton(onClick = { stopVpn() }, enabled = canStop) {
                        Text("Stop VPN")
                    }
                }
                Text(text = "State: $vpnState  | Proxy: ${if (proxyOnline) "ONLINE" else "OFFLINE"}")
                if (vpnState == "CONNECTED") {
                    val rxRate = humanBits(rxBps)
                    val txRate = humanBits(txBps)
                    val rxT = humanBytes(rxTotal)
                    val txT = humanBytes(txTotal)
                    Text("Down: $rxRate/s  Up: $txRate/s")
                    Text("Total ↓ $rxT  ↑ $txT")
                }
            }
        }
    }

    private fun ensureVpnPermissionThenStart() {
        if (Build.VERSION.SDK_INT >= 33) {
            requestPermissions(arrayOf(android.Manifest.permission.POST_NOTIFICATIONS), 1)
        }
        val intent = VpnService.prepare(this)
        if (intent != null) {
            // Need to show the "Allow VPN connection" system dialog
            vpnPrep.launch(intent)
        } else {
            // Already granted before
            startVpnService()
        }
    }

    /**
     * Actually starts the VPN service using the ACTION_START we defined
     * in AppVpnService.
     */
    private fun startVpnService() {
        val host = proxyHost.trim()
        val port = proxyPort.toIntOrNull() ?: 8281
        val pkg  = targetPkg.trim()

        // persist values for BootReceiver if user enabled autostart
        savePrefs()

        val i = Intent(this, AppVpnService::class.java).apply {
            action = AppVpnService.ACTION_START
            putExtra(AppVpnService.EXTRA_PROXY_HOST, host)
            putExtra(AppVpnService.EXTRA_PROXY_PORT, port)
            putExtra(AppVpnService.EXTRA_TARGET_PKG, pkg)
        }
        isStarting = true
        startService(i)
    }

    /**
     * Sends ACTION_STOP so the service can clean up and call stopSelf().
     */
    private fun stopVpn() {
        val i = Intent(this, AppVpnService::class.java).apply {
            action = AppVpnService.ACTION_STOP
        }
        startService(i)
    }

    private val stateReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context?, intent: Intent?) {
            when (intent?.action) {
                AppVpnService.ACTION_STATE -> {
                    val s = intent.getStringExtra(AppVpnService.EXTRA_STATE) ?: return
                    vpnState = s
                    if (s == "CONNECTED" || s == "INACTIVE" || s == "ERROR") {
                        isStarting = false
                    }
                }
                AppVpnService.ACTION_METRICS -> {
                    rxBps = intent.getLongExtra(AppVpnService.EXTRA_RX_BPS, 0L)
                    txBps = intent.getLongExtra(AppVpnService.EXTRA_TX_BPS, 0L)
                    rxTotal = intent.getLongExtra(AppVpnService.EXTRA_RX_TOTAL, 0L)
                    txTotal = intent.getLongExtra(AppVpnService.EXTRA_TX_TOTAL, 0L)
                    proxyOnline = intent.getBooleanExtra(AppVpnService.EXTRA_PROXY_ONLINE, false)
                }
            }
        }
    }

    private fun humanBytes(v: Long): String = when {
        v < 1024 -> "$v B"
        v < 1024*1024 -> String.format("%.1f KB", v / 1024.0)
        v < 1024*1024*1024 -> String.format("%.1f MB", v / (1024.0*1024))
        else -> String.format("%.2f GB", v / (1024.0*1024*1024))
    }

    private fun humanBits(bps: Long): String {
        val v = bps.toDouble()
        return when {
            v < 1000 -> String.format("%.0f b", v)
            v < 1000_000 -> String.format("%.1f Kb", v/1000)
            v < 1000_000_000 -> String.format("%.1f Mb", v/1000_000)
            else -> String.format("%.2f Gb", v/1000_000_000)
        }
    }

    private fun savePrefs() {
        val port = proxyPort.toIntOrNull() ?: 8281
        prefs.edit()
            .putString("proxy_host", proxyHost.trim())
            .putInt("proxy_port", port)
            .putString("target_pkg", targetPkg.trim())
            .putBoolean("autostart_enabled", autostart)
            .apply()
    }
}

package com.example.blaulichtproxy

import android.app.Activity
import android.content.Intent
import android.net.VpnService
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

class MainActivity : ComponentActivity() {

    // UI state for fields
    private var proxyHost by mutableStateOf("192.168.1.50")          // later: your DDNS name
    private var proxyPort by mutableStateOf("1080")
    private var targetPkg by mutableStateOf("net.ut11.ccmp.blaulicht")

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
        setContent { Ui() }
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
                Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                    Button(onClick = { ensureVpnPermissionThenStart() }) {
                        Text("Start VPN")
                    }
                    OutlinedButton(onClick = { stopVpn() }) {
                        Text("Stop VPN")
                    }
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
        val port = proxyPort.toIntOrNull() ?: 1080
        val pkg  = targetPkg.trim()

        val i = Intent(this, AppVpnService::class.java).apply {
            action = AppVpnService.ACTION_START
            putExtra(AppVpnService.EXTRA_PROXY_HOST, host)
            putExtra(AppVpnService.EXTRA_PROXY_PORT, port)
            putExtra(AppVpnService.EXTRA_TARGET_PKG, pkg)
        }
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
}

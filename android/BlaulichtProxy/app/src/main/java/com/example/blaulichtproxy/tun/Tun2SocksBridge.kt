package com.example.blaulichtproxy.tun

import android.util.Log
import mobile.Mobile
import mobile.TunnelHandle

object Tun2SocksBridge {
    private const val TAG = "Tun2SocksBridge"

    @JvmStatic
    fun start(fd: Int, host: String, port: Int): TunnelHandle {
        Log.d(TAG, "start(fd=$fd, host=$host, port=$port)")
        return try {
            // NOTE: .toLong() to match gomobile's (long, String, long) signature
            val handle = Mobile.startSocksTunnel(fd.toLong(), host, port.toLong())
            Log.d(TAG, "start OK, handle=$handle")
            handle
        } catch (e: Exception) {
            Log.e(TAG, "start FAILED", e)
            throw e
        }
    }

    @JvmStatic
    fun stop(handle: TunnelHandle?) {
        if (handle == null) {
            Log.d(TAG, "stop: null handle, nothing to do")
            return
        }
        try {
            Mobile.stopTunnel(handle)
            Log.d(TAG, "stop: done")
        } catch (e: Exception) {
            Log.e(TAG, "stop FAILED", e)
        }
    }
}

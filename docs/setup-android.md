# Android Setup (Per-App VPN)

The Android client is a VpnService-based app that forwards only the Blaulicht app’s traffic through a SOCKS5 proxy (mitmproxy) using a tun2socks AAR.

> Note: The tun2socks AAR is assumed to be prebuilt elsewhere and not included here.

## Steps

1. Import the project
   - Open Android Studio.
   - File → Open… → select `android/BlaulichtProxy/` (this folder is a placeholder for your actual project).

2. Add the tun2socks AAR
   - Place your `socks-tun2socks.aar` in the app module’s libs folder, e.g. `android/BlaulichtProxy/app/libs/socks-tun2socks.aar`.
   - In `build.gradle`, add:
     - `repositories { flatDir { dirs 'libs' } }`
     - `implementation(name: 'socks-tun2socks', ext: 'aar')`

3. Configure per-app VPN
   - Use `VpnService` to establish the TUN interface.
   - Configure the per-app restriction so only the Blaulicht package’s UID is routed (App Exclusions/Inclusions depending on API level).
   - Use tun2socks to forward TCP connections to a SOCKS5 endpoint.

4. Proxy settings (UI)
   - Expose a simple settings screen to set:
     - Host: `<DOMAIN_OR_DDNS>` of your server
     - Port: `8281` (or the configured SOCKS5 port)

5. Trust the mitmproxy CA
   - Install the mitmproxy CA certificate on the device so TLS interception works.
   - On modern Android, user-added CAs aren’t trusted by apps by default; you may need to opt-in via Network Security Config for debugging, or use a rooted device/emulator for testing.

6. Build & Install
   - Build the APK and install it on your test device.
   - Start the VPN and verify that only the Blaulicht app’s traffic is routed via mitmproxy.

## Notes / TODOs

- Consider adding a kill-switch for when the proxy is unreachable.
- Ensure the app doesn’t capture system/other apps unless intended.
- Handle IPv6 if your network or app uses it.
- Provide a minimal log view for troubleshooting.

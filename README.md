# blaulicht-garage-proxy

A DIY setup that routes the HTTPS traffic of one Android app ("Blaulicht") through a self-hosted mitmproxy. When the proxy detects a specific request pattern, it triggers a local web service which in turn instructs an ESP8266 to pulse a relay that opens a garage door.

This repo contains the scaffolding for all pieces: Android per-app VPN client, a mitmproxy addon, a small Python "garage-controller" service, ESP8266 firmware stubs, and infrastructure templates (systemd, nginx, firewall notes). It’s intended for experimentation and learning—not production-grade security.

## Data flow

```
 [Blaulicht app]
        |
        v
 Android per-app VPN (VpnService) + tun2socks
        |
        v
     SOCKS5
        |
        v
     mitmproxy  --(local HTTP trigger)-->  garage-controller  --(poll)-->  ESP8266  -->  relay
        ^                                                                             (garage door)
        |                                                                                   
   TLS intercepted (phone trusts mitm CA)
```

Or in one line:

```
Blaulicht app → Android per-app VPN + tun2socks → SOCKS5 → mitmproxy → local HTTP trigger → garage-controller → ESP8266 → garage door relay
```

## Components

- Android per-app VPN client
  - VpnService-based app that forwards only the target app’s traffic via a SOCKS5 proxy (mitmproxy) using a tun2socks AAR.
- mitmproxy addon
  - Inspects HTTP(S) flows and fires a local HTTP POST when a pattern is detected.
- garage-controller
  - Tiny Python web service with two endpoints: `/trigger` (local-only) and `/poll` (Internet-exposed via nginx). Holds per-device tokens and pending commands.
- ESP8266 firmware
  - Connects to Wi‑Fi and periodically polls `/garage/poll`. When it receives `{"cmd": "open"}`, it pulses a relay GPIO.

## Quickstart

1) Server (Ubuntu)
- Install and configure mitmproxy and the garage-controller.
- Configure nginx to expose only `/garage/poll` over HTTPS.

2) Android
- Build and install the per-app VPN client (Android Studio). Point it at the server’s SOCKS5 port. Install the mitmproxy CA on the device.

3) ESP8266
- Flash the sketch, set Wi‑Fi + device token, and point it to your server’s `/garage/poll` URL.

Security note: This is a hobby project. Keep the attack surface small, use long random tokens, and prefer TLS everywhere—but don’t treat it like a bank vault.

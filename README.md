# blaulicht-garage-proxy (MQTT)

A DIY setup that routes the HTTPS traffic of one Android app ("Blaulicht") through a self-hosted mitmproxy. When a specific flow pattern is detected, the mitmproxy addon publishes an MQTT command to a local Mosquitto broker (TLS + auth). An ESP8266 subscribed to that topic pulses a relay to open the garage.

This repo includes: Android per-app VPN client, a mitmproxy addon with MQTT publishing, ESP8266 RTOS-based firmware with MQTT over TLS, and infrastructure templates (systemd, firewall, MQTT broker hardening). Intended for experimentation and learning.

## Data flow (MQTT)

```
[Blaulicht app]
      |
      v
Android per-app VPN (VpnService + tun2socks) → SOCKS5 → mitmproxy --(MQTT/TLS publish)--> Mosquitto → ESP8266 → relay
      ^                                 TLS intercept (trusted CA)                 ^ TLS + auth            (garage door)
```

## Components

- Android per-app VPN client
  - VpnService-based app that forwards only the target app’s traffic via a SOCKS5 proxy (mitmproxy) using a tun2socks AAR.
- mitmproxy addon
  - Detects the flow trigger and publishes `open` to `garage/cmd` over TLS to Mosquitto on 8883.
  - Configurable via environment variables (`MQTT_HOST`, `MQTT_USER/PASS`, `TRIGGER_*`). See `infra/systemd/mitmproxy.service`.
- Mosquitto broker
  - TLS 1.2+, per-user passwords in `/etc/mosquitto/passwd`, and minimal permissions. See `docs/setup-mqtt-broker.md`.
- ESP8266 firmware
  - Connects to Wi‑Fi and MQTT over TLS, subscribes to `garage/cmd`, and publishes `garage/status`. Embed the CA in `lib/mqtt/src/mqtt.c`.

## Quickstart

1) Server (Ubuntu)
- Install Mosquitto and set up TLS + users: `docs/setup-mqtt-broker.md`.
- Deploy mitmproxy addon and set MQTT env: `docs/setup-server-ubuntu.md`.

2) Android
- Build and install the per-app VPN client (Android Studio). Point it to the server’s SOCKS5 port. Install the mitmproxy CA on the device.

3) ESP8266
- Open `esp8266/Garage-Controller` in PlatformIO. Set Wi‑Fi + MQTT config (including CA). Build and flash.

Security note: Use strong per-client credentials. Keep the broker LAN-only or restricted. Self-signed CA is fine for local deployments; keep the private key secure.

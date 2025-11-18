# Architecture (MQTT)

This document describes the updated MQTT-based design. mitmproxy detects the app’s flow pattern and publishes an MQTT command to a local Mosquitto broker (TLS + auth). The ESP8266 subscribes to that command topic and publishes status back.

## High-Level Diagram

```
(Android Phone)
+-------------------+      SOCKS5        +------------+       MQTT (TLS)       +--------------------+      Relay
| Blaulicht App     | ---> per-app VPN --|  mitmproxy |--pub--> garage/cmd ---> |   Mosquitto Broker | --->  ESP8266
+-------------------+      (tun2socks)   +------------+<-sub-- garage/status <--+ (localhost:8883)   |   (subscribe cmd,
   | HTTPS Req.             | TLS intercept (CA trusted)                        ^ auth+TLS          |    publish status)
   v                        | inspects flows                                    |                   v
 Normal app logic               | addon publishes command                           |               Garage Door
```

## Step-by-Step Data Flow

1. The Blaulicht app sends HTTPS requests as part of its normal operation.
2. The Android per-app VPN (VpnService + tun2socks) forwards ONLY that app’s TCP streams to mitmproxy (SOCKS5).
3. mitmproxy (trusted CA on the phone) sees HTTP metadata and matches a configured trigger.
4. The mitmproxy addon publishes `GARAGE_CMD_PAYLOAD` (default `open`) to topic `garage/cmd` over TLS to the local Mosquitto broker on 8883.
5. The ESP8266 (RTOS SDK + mbedTLS) is connected to the same broker, subscribes to `garage/cmd`, and when it receives `open`, pulses the relay.
6. The ESP8266 publishes `garage/status` updates (e.g., `idle`, `opening`, `opened`, `closed`) that mitmproxy or dashboards can consume.

## Responsibilities

- mitmproxy: Detects pattern and publishes MQTT command. No long-term state.
- Mosquitto: Authenticates clients, terminates TLS, routes topics.
- ESP8266: MQTT client; subscribes to command topic and publishes status.

## Trust Boundaries

- Broker listens on `localhost:8883` or LAN-restricted 8883/TCP with TLS 1.2+.
- Strong username/password per client; broker runs as locked-down system user.
- Self-signed private CA recommended for local deployments; embed CA in firmware.

## Open Items / TODOs

- Fine-tune mitmproxy trigger conditions (SNI, CONNECT host:port, URL substring).
- Debounce commands (mitmproxy has cooldown; firmware may also enforce cooldown).
- Optional: mTLS or per-device topics like `garage/door1/cmd`.

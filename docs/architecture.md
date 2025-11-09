# Architecture

This document expands on how the pieces interact to get from an app’s HTTPS request to a garage door relay pulse.

## High-Level Diagram

```
 (Android Phone)
 +-------------------+      SOCKS5        +------------+    Local HTTP     +------------------+    Poll (HTTPS)    +-----------+
 | Blaulicht App     | ---> per-app VPN --|  mitmproxy |--> /trigger (POST)| garage-controller |<-- /poll (GET) ---| ESP8266   |--> Relay Pulse
 +-------------------+      (tun2socks)   +------------+                    +------------------+                    +-----------+
        | HTTPS Req.              | TLS intercept (mitm CA trusted)                  ^ holds command state                |
        v                         | inspects flows                                  |                                     v
  Normal app logic                | addon triggers open                             | JSON {cmd: open}                    Garage Door
```

## Step-by-Step Data Flow

1. The Blaulicht app sends HTTPS requests as part of its normal operation.
2. The Android per-app VPN (VpnService + tun2socks AAR) captures ONLY that app’s traffic and forwards raw TCP streams via SOCKS5 to mitmproxy on the server.
3. mitmproxy terminates TLS (because the phone trusts its CA certificate) and exposes HTTP request details to the addon.
4. The custom addon examines each flow (URL, headers, payload). When a defined pattern matches (TODO: specify pattern), it POSTs JSON to `http://127.0.0.1:5001/trigger`.
5. The `garage-controller` service records a pending command for the requested device (e.g. `{cmd: "open"}`).
6. The ESP8266 device periodically polls `https://<DDNS_OR_DOMAIN>/garage/poll?device=<id>&token=<secret>` (proxied by nginx to the controller on `127.0.0.1:5002`).
7. If a pending command exists, the controller returns `{ "cmd": "open" }` then clears it; otherwise `{ "cmd": "idle" }`.
8. The ESP8266 receives `open`, toggles a relay GPIO for a short pulse, then resumes polling until the next command.

## Responsibilities Separation

- mitmproxy: Only inspects HTTP flows. Does NOT decide access control or device tokens.
- garage-controller: Owns device registry (tokens) and pending command state. All open-door logic lives here.
- ESP8266 firmware: Polls the public endpoint securely (TLS via nginx). Acts only when instructed. Keeps Wi-Fi credentials and its token.

## Trust Boundaries

- Public Internet: Only `nginx:443 -> /garage/poll` should be exposed.
- Localhost on server: `/trigger` endpoint (mitmproxy addon to controller) is bound to `127.0.0.1`.
- Device tokens: Stored in `config.yml` (not committed—only example provided). Tokens must be long random strings.

## Open Questions / TODOs

- Precise trigger condition inside the mitmproxy addon (URL, JSON body fragment, header match?).
- Rate limiting or debouncing triggers to prevent repeated openings.
- Expiration or replay protection for commands (e.g. TTL after trigger).
- Logging and observability: minimal structured logs? metrics?
- Hardening: fail-closed if config missing, restrict /poll request rate.

## Future Enhancements (Ideas)

- Optional authentication layer or mTLS between ESP8266 and server.
- Web UI for manual override / door status.
- Use websockets instead of polling (would require different connectivity assumptions).
- Integrate simple anomaly detection (e.g. too many triggers in short time).

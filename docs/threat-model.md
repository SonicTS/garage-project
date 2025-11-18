# Threat Model (MQTT)

Plain-language overview of what we’re defending against and what is out of scope for this hobby setup.

## Goals / What We Try To Protect Against

- Random Internet scanners or bots hitting open ports.
- Opportunistic attackers targeting open MQTT (8883) or SOCKS5 ports.
- Accidental or unauthorized triggering of the garage door via MQTT command injection.
- Simple replay scenarios via repeated publishes if cooldowns are bypassed.

## Non-Goals / Out of Scope

- Highly resourced or motivated attackers targeting the system directly.
- Physical compromise of the ESP8266, relay, or wiring (someone steals the board).
- Advanced MITM attacks against TLS (assumes correct certificate management and no device tampering).
- Protecting against someone who already has root on the server.

## Assets

- Ability to open the garage door.
- MQTT credentials stored in `/etc/mosquitto/passwd`.
- Integrity of command flow between mitmproxy addon and mosquitto broker.

## Attack Surfaces

- MQTT over TLS on 8883 (should be LAN-only or source-restricted).
- SOCKS5 port for the phone (restricted firewall rules recommended).

## Mitigations

- MQTT broker is LAN-only or source-restricted; TLS 1.2+ with strong per-client passwords.
- Separate Unix users `mitmproxy` and `mosq` to reduce lateral impact.
- Firewall: restrict SOCKS5 and 8883; allow SSH.
- TLS on MQTT; self-signed CA with ESP8266 embedding; server key permissions locked down.
- mitmproxy cooldown on publishes to reduce accidental repeats.

## Residual Risks / Trade-offs

- Passwords in mosquitto passwd; protect file mode; consider per-device accounts.
- ESP8266 TLS validation relies on embedded CA; keep CA private key offline.
- No rate limiting on MQTT by default—consider connection limits and auth lockouts.

## Future Hardening Ideas

- Consider mTLS or per-device topics.
- Implement limited command history or deduplication on firmware side.
- Add systemd sandboxing directives for mitmproxy and mosquitto.
- Monitor logs for unusual connection attempts or repeated publishes.

## Summary

Security posture: “Reasonable for a hobby project.” It reduces exposure, uses TLS and per-client credentials, and isolates components. It is not designed for high-assurance environments—treat it as a learning platform.

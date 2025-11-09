# Threat Model

Plain-language overview of what we’re defending against and what is out of scope for this hobby setup.

## Goals / What We Try To Protect Against

- Random Internet scanners or bots hitting open ports.
- Opportunistic attackers discovering the `/garage/poll` endpoint.
- Accidental or unauthorized triggering of the garage door via forged poll responses.
- Simple replay scenarios (e.g. repeatedly receiving an `open` command if not cleared).

## Non-Goals / Out of Scope

- Highly resourced or motivated attackers targeting the system directly.
- Physical compromise of the ESP8266, relay, or wiring (someone steals the board).
- Advanced MITM attacks against TLS (assumes correct certificate management and no device tampering).
- Protecting against someone who already has root on the server.

## Assets

- Ability to open the garage door.
- Device tokens stored in `config.yml`.
- Integrity of command flow between mitmproxy addon and garage-controller.

## Attack Surfaces

- Public HTTPS endpoint `/garage/poll`.
- SOCKS5 port exposed for the phone (restricted firewall rules recommended).
- Local internal `/trigger` endpoint (should be bound to 127.0.0.1 only).

## Mitigations

- Only `/garage/poll` is exposed publicly; `/trigger` remains localhost.
- Long random per-device tokens (not human guessable).
- Separate Unix users `mitmproxy` and `garage` to reduce lateral impact.
- Firewall: restrict SOCKS5 port, allow only needed HTTPS and SSH.
- TLS via nginx for ESP8266 polling (protects token in transit).
- Simple state clearing after command use (prevents indefinite repeated opens).

## Residual Risks / Trade-offs

- Tokens stored in plain text config on disk (acceptable for hobby use; could be encrypted or in a vault for production).
- ESP8266 may use less robust TLS validation (certificate pinning recommended for stronger security).
- Rate limiting not implemented yet—potential brute-force attempts on `/garage/poll` could occur.

## Future Hardening Ideas

- Add nginx rate limiting / fail2ban.
- Use mutual TLS or signed requests from device.
- Implement limited command history with timestamps and TTL.
- Add systemd sandboxing directives (ProtectHome, PrivateTmp, etc.).
- Monitor logs for unusual poll frequency or failed token attempts.

## Summary

Security posture: “Reasonable for a hobby project.” It reduces exposure, uses long tokens, and isolates components. It is not designed for high-assurance environments or motivated attackers—treat it as a learning platform.

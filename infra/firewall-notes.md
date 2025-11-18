# Firewall Notes (MQTT)

Example `ufw` rules for the MQTT-based setup. Adjust to your risk appetite and network layout.

## Goals

- Allow MQTT over TLS on 8883 (LAN-only if possible).
- Limit SOCKS5 port (mitmproxy) to the phone/VPN source IP(s).
- Allow SSH for maintenance (optionally restricted to home IPs).

## Example UFW Setup

```bash
sudo ufw default deny incoming
sudo ufw default allow outgoing

# Allow SSH (restrict to known source if possible)
sudo ufw allow 22/tcp
# e.g.: sudo ufw allow from <HOME_IP> to any port 22 proto tcp

# Allow MQTT over TLS (broker)
sudo ufw allow 8883/tcp

# Restrict SOCKS5 port to phone IP or VPN CIDR
sudo ufw allow proto tcp from <PHONE_IP> to any port 8281
# Alternatively: sudo ufw allow from <VPN_SUBNET>/24 to any port 8281 proto tcp

sudo ufw enable
sudo ufw status verbose
```

## Additional Considerations

- Keep system packages updated; a firewall doesn’t patch vulnerabilities.
- Avoid exposing 8883 to the Internet; prefer LAN-only or source-restricted rules.
- If you previously used nginx/HTTPS for polling, remove those rules.

## Non-UFW Alternatives

- `nftables` or `iptables` directly for more granular control.
- Cloud firewall (if hosted on a cloud provider) restricting ingress prior to host.

## TODOs

- Replace `<PHONE_IP>` with your device’s stable IP or run a VPN.
- Add logging review process (e.g., weekly audit of access patterns).

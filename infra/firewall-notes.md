# Firewall Notes

These notes provide example approaches using `ufw` (Uncomplicated Firewall) on Ubuntu. Adjust rules to your own risk appetite and network layout.

## Goals

- Expose only HTTPS for `/garage/poll`.
- Limit SOCKS5 port (mitmproxy) to specific source IP(s) (e.g., your phone or VPN range).
- Allow SSH for maintenance (optionally restricted to home IPs).

## Example UFW Setup

```bash
sudo ufw default deny incoming
sudo ufw default allow outgoing

# Allow SSH (restrict to known source if possible)
sudo ufw allow 22/tcp
# e.g.: sudo ufw allow from <HOME_IP> to any port 22 proto tcp

# Allow HTTPS for ESP8266 polling
sudo ufw allow 443/tcp

# Restrict SOCKS5 port to phone IP or VPN CIDR
sudo ufw allow proto tcp from <PHONE_IP> to any port 8281
# Alternatively: sudo ufw allow from <VPN_SUBNET>/24 to any port 8281 proto tcp

sudo ufw enable
sudo ufw status verbose
```

## Additional Considerations

- Consider fail2ban or rate limiting at nginx for repeated `/garage/poll` hits.
- Keep system packages updated; a firewall doesn’t patch vulnerabilities.
- Avoid exposing `:5001` (trigger endpoint) beyond localhost.
- Consider closing port 80 unless needed for ACME/Certbot HTTP challenges (then re-lock after certificate issuance).

## Non-UFW Alternatives

- `nftables` or `iptables` directly for more granular control.
- Cloud firewall (if hosted on a cloud provider) restricting ingress prior to host.

## TODOs

- Replace `<PHONE_IP>` with your device’s stable IP or run a VPN.
- Add logging review process (e.g., weekly audit of access patterns).

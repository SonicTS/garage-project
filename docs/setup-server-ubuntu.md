# Server Setup (Ubuntu)

This guide walks through preparing an Ubuntu server to host mitmproxy and the garage-controller service. Commands are intentionally generic—adjust paths, versions, and domains for your environment.

> TODO: Replace placeholders like `<DOMAIN_OR_DDNS>`, ports, and file paths with real values.

## 1. Prerequisites

- Ubuntu 22.04 LTS (or similar) with root/sudo access.
- Stable public DNS name / DDNS for the ESP8266 to reach (`<DOMAIN_OR_DDNS>`).
- Ability to install packages and open firewall ports.

## 2. Create Service Users

Create dedicated unprivileged users to run each component:

```bash
sudo adduser --system --group mitmproxy
sudo adduser --system --group garage
```

Rationale: Least privilege separation. Each process has its own user.

## 3. Install Dependencies

```bash
sudo apt update
sudo apt install -y python3 python3-venv python3-pip nginx mitmproxy
```

Optional: `tmux`, `git`, `ufw`, `fail2ban` for operational convenience.

## 4. Deploy garage-controller

Directory layout suggestion:

```bash
sudo mkdir -p /opt/garage-controller
sudo chown garage:garage /opt/garage-controller
```

Copy the repository subtree (from your development machine or git clone) into `/opt/garage-controller`. Example:

```bash
sudo rsync -av blaulicht-garage-proxy/garage-controller/ /opt/garage-controller/
```

Create and populate virtual environment:

```bash
sudo -u garage python3 -m venv /opt/garage-controller/venv
sudo -u garage /opt/garage-controller/venv/bin/pip install -r /opt/garage-controller/requirements.txt
```

Configuration:

```bash
sudo -u garage cp /opt/garage-controller/config.example.yml /opt/garage-controller/config.yml
# Edit /opt/garage-controller/config.yml and insert long random tokens.
```

## 5. Deploy mitmproxy

Run mitmproxy as the `mitmproxy` user. The addon script should reside under an owned directory:

```bash
sudo mkdir -p /opt/blaulicht/mitmproxy
sudo chown mitmproxy:mitmproxy /opt/blaulicht/mitmproxy
sudo rsync -av blaulicht-garage-proxy/mitmproxy/ /opt/blaulicht/mitmproxy/
```

Test manually (optional) without persistent logging:

```bash
sudo -u mitmproxy mitmdump --mode socks5 --listen-host 0.0.0.0 --listen-port 1080 -s /opt/blaulicht/mitmproxy/garage_trigger.py
```

Enable file logging (recommended for pattern analysis):

```bash
sudo -u mitmproxy mitmdump --mode socks5 --listen-host 0.0.0.0 --listen-port 1080 \
	-s /opt/blaulicht/mitmproxy/garage_trigger.py \
	--set logfile=/var/log/mitmproxy/mitmproxy.log --set console_eventlog_verbosity=info
```

Create and own the log directory (if not using systemd unit to do it):

```bash
sudo mkdir -p /var/log/mitmproxy
sudo chown mitmproxy:mitmproxy /var/log/mitmproxy
```

> TODO: Confirm the Android app’s per-app VPN points to this server’s IP and port 1080.

## 6. systemd Units

Copy unit files:

```bash
sudo cp blaulicht-garage-proxy/infra/systemd/mitmproxy.service /etc/systemd/system/
sudo cp blaulicht-garage-proxy/infra/systemd/garage-controller.service /etc/systemd/system/
```

Reload and enable:

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now mitmproxy.service
sudo systemctl enable --now garage-controller.service
```

Check status:

```bash
systemctl status mitmproxy.service
systemctl status garage-controller.service
```

## 7. nginx Reverse Proxy

Goal: Expose only `/garage/poll` over HTTPS; keep `/trigger` private on localhost.

Place the config:

```bash
sudo cp blaulicht-garage-proxy/infra/nginx/garage.conf /etc/nginx/sites-available/garage.conf
sudo ln -s /etc/nginx/sites-available/garage.conf /etc/nginx/sites-enabled/garage.conf
```

Obtain TLS certificates (e.g. via Certbot):

```bash
sudo certbot certonly --nginx -d <DOMAIN_OR_DDNS>
```

Test config and reload:

```bash
sudo nginx -t
sudo systemctl reload nginx
```

## 8. Firewall / UFW

Basic example (adjust to your threat model):

```bash
sudo ufw default deny incoming
sudo ufw default allow outgoing
sudo ufw allow proto tcp from <YOUR_PHONE_IP> to any port 1080   # SOCKS5 (restricted)
sudo ufw allow 443/tcp                                         # HTTPS for /garage/poll
sudo ufw allow ssh                                             # Consider restricting source IPs
sudo ufw enable
```

Refer to `infra/firewall-notes.md` for more narrative guidance.

## 9. mitmproxy CA on Android

Export mitmproxy CA certificate (mitmproxy generates it on first run in `~/.mitmproxy/`). Install it as a user CA on the Android device (Developer options may be required on newer versions).

## 10. Monitoring & Logs (Optional)

- Tail logs (systemd journal):
	- `journalctl -u mitmproxy -f`
	- `journalctl -u garage-controller -f`
- File logs (if using `--set logfile=/var/log/mitmproxy/mitmproxy.log`):
	- `tail -f /var/log/mitmproxy/mitmproxy.log`
- Add log rotation (example `/etc/logrotate.d/mitmproxy`):

```bash
sudo tee /etc/logrotate.d/mitmproxy >/dev/null <<'EOF'
/var/log/mitmproxy/mitmproxy.log {
		daily
		rotate 7
		compress
		missingok
		notifempty
		create 640 mitmproxy mitmproxy
}
EOF
```

- Consider minimal structured logging or filtering sensitive headers before writing to disk.

## 11. TODO / Hardening Ideas

- Rate limit `/garage/poll` at nginx.
- Fail closed if config file missing or tokens invalid.
- Add systemd `ProtectSystem=full` / `NoNewPrivileges=yes` etc. (advanced hardening).
- Consider running mitmproxy with a fixed certificate set if needed.

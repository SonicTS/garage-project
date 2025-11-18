# Server Setup (Ubuntu) – mitmproxy + Mosquitto (MQTT)

This guide sets up mitmproxy and a secure MQTT broker (Mosquitto with TLS + auth). The ESP8266 will subscribe to commands and publish status via MQTT. No HTTP polling service is required anymore.

## 1. Prerequisites

- Ubuntu 22.04 LTS (or similar) with root/sudo access.
- Stable public DNS name / DDNS for the ESP8266 to reach (`<DOMAIN_OR_DDNS>`).
- Ability to install packages and open firewall ports.

## 2. Create Service Users

Create dedicated unprivileged users to run each component:

```bash
sudo adduser --system --group mitmproxy
sudo adduser --system --group mosq
```

Rationale: Least privilege separation. Each process has its own user.

## 3. Install Dependencies

```bash
sudo apt update
sudo apt install -y python3 python3-venv python3-pip mitmproxy mosquitto mosquitto-clients ufw
```

Optional: `tmux`, `git`, `ufw`, `fail2ban` for operational convenience.

## 4. Install and Secure Mosquitto (TLS + Auth)

Follow the dedicated guide in `docs/setup-mqtt-broker.md` (self-signed CA recommended for local networks). Summary:

- Create `/etc/mosquitto/certs` and generate CA + server certs.
- Create `/etc/mosquitto/passwd` and add users (e.g., `espclient`, `mitmclient`).
- Add `/etc/mosquitto/conf.d/secure.conf` with TLS listener on 8883, `allow_anonymous false`, and `user mosq`.
- `sudo systemctl restart mosquitto` and test with `mosquitto_sub/pub` using `--cafile` and credentials.

## 5. Deploy mitmproxy

Run mitmproxy as the `mitmproxy` user. The addon script should reside under an owned directory:

```bash
sudo mkdir -p /opt/blaulicht/mitmproxy
sudo chown -R mitmproxy:mitmproxy /opt/blaulicht/mitmproxy
# (Old approach) If using a confdir inside /opt: sudo -u mitmproxy mkdir -p /opt/blaulicht/mitmproxy/.mitmproxy
# New recommended approach relies on systemd StateDirectory=mitmproxy which auto-creates /var/lib/mitmproxy
sudo rsync -av ./mitmproxy/ /opt/blaulicht/mitmproxy/
sudo -u mitmproxy python3 -m venv /opt/blaulicht/mitmproxy/venv
sudo -u mitmproxy /opt/blaulicht/mitmproxy/venv/bin/pip install -r /opt/blaulicht/mitmproxy/requirements.txt
```

Test manually (optional) – explicitly set a writable confdir because system users often have HOME=/nonexistent:

```bash
sudo -u mitmproxy mitmdump --set confdir=/var/lib/mitmproxy --mode socks5 --listen-host 0.0.0.0 --listen-port 8281 -s /opt/blaulicht/mitmproxy/garage_trigger.py
```

> TODO: Confirm the Android app’s per-app VPN points to this server’s IP and port 8281.

## 6. systemd Units

Copy unit files:

```bash
sudo cp ./infra/systemd/mitmproxy.service /etc/systemd/system/
## 6.1 MQTT credentials for mitmproxy addon

Create environment file for the addon to reach the broker securely:

```bash
sudo mkdir -p /etc/blaulicht
sudo tee /etc/blaulicht/mqtt.env >/dev/null <<'EOF'
MQTT_HOST=127.0.0.1
MQTT_PORT=8883
MQTT_USER=mitmclient
MQTT_PASS=REPLACE_WITH_STRONG_PASSWORD
MQTT_CAFILE=/etc/mosquitto/certs/ca.crt
GARAGE_CMD_TOPIC=garage/cmd
GARAGE_CMD_PAYLOAD=open
# Example trigger: exact SNI match, or URL contains, or CONNECT host/port
#TRIGGER_SNI=
#TRIGGER_URL_CONTAINS=
#TRIGGER_CONNECT_HOST=
#TRIGGER_CONNECT_PORT=
PUBLISH_COOLDOWN_SEC=10
EOF
```

Set readable permissions for mitmproxy:

```bash
sudo chown root:mitmproxy /etc/blaulicht/mqtt.env
sudo chmod 640 /etc/blaulicht/mqtt.env
```
```

With the updated unit file including `StateDirectory=mitmproxy`, systemd will create `/var/lib/mitmproxy` automatically at start. No manual confdir creation is needed. If you are on the old unit file, create the legacy path manually.

Reload and enable:

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now mitmproxy.service
```

Check status:

```bash
systemctl status mitmproxy.service
```

### Allowing mitmproxy to manage Mosquitto users (optional)

Clients do not need to read `/etc/mosquitto/passwd`; they just authenticate. If you want the `mitmproxy` user to run `mosquitto_passwd` (to add/change users) without full root, prefer `sudoers` for that command:

```bash
sudo visudo -f /etc/sudoers.d/mitmproxy-mosquitto
```

Add:

```
mitmproxy ALL=(root) NOPASSWD: /usr/bin/mosquitto_passwd *
```

If you instead want to grant read access to the password file (not recommended), you can loosen permissions:

```bash
sudo chgrp mitmproxy /etc/mosquitto/passwd
sudo chmod 640 /etc/mosquitto/passwd
```

Security note: the broker still verifies passwords internally; reading the file is sensitive and should be avoided.
```

## 7. Firewall / UFW

## 8. Firewall / UFW

Basic example (adjust to your threat model):

```bash
sudo ufw default deny incoming
sudo ufw default allow outgoing
sudo ufw allow proto tcp from <YOUR_PHONE_IP> to any port 8281   # SOCKS5 (restricted)
sudo ufw allow 8883/tcp                                         # MQTT over TLS
sudo ufw allow ssh                                             # Consider restricting source IPs
sudo ufw enable
```

Refer to `infra/firewall-notes.md` for more narrative guidance.

## 8. mitmproxy CA on Android

Export mitmproxy CA certificate (mitmproxy generates it on first run in `~/.mitmproxy/`). Install it as a user CA on the Android device (Developer options may be required on newer versions).

## 9. Monitoring & Logs (Optional)

- Tail logs (systemd journal):
	- `journalctl -u mitmproxy -f`
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

## 10. TODO / Hardening Ideas

- Consider mTLS on MQTT if device can handle certs.
- Add systemd `ProtectSystem=full` / `NoNewPrivileges=yes` etc. (advanced hardening).
- Consider running mitmproxy with a fixed certificate set if needed.

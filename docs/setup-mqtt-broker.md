# Secure MQTT Broker Setup on Ubuntu (Mosquitto + TLS + Auth)

This guide installs a secure Mosquitto broker with TLS, strong authentication, a dedicated system user, strict permissions, and firewall rules. It’s compatible with ESP8266 (RTOS SDK + mbedTLS) and standard MQTT clients.

## 1) Install Mosquitto Broker & Clients

```bash
sudo apt update
sudo apt install mosquitto mosquitto-clients
```

Verify the service:

```bash
sudo systemctl status mosquitto
```

## 2) Create a Dedicated Mosquitto User

Run Mosquitto as a locked-down system user (create user + group together):

```bash
sudo adduser --system --no-create-home --disabled-password --group mosq
```

## 3) Create Secure TLS Certificates (Self-signed CA)

Use the provided generator script (creates a private CA and a server cert with proper SANs and permissions). Edit the SAN config first, then run the generator:

```bash
sudo mkdir -p /etc/mosquitto/certs
# Copy the SAN config and edit placeholders (LAN IP, hostnames)
sudo cp ./infra/mosquitto/openssl-san.cnf /etc/mosquitto/certs/openssl-san.cnf
sudo nano /etc/mosquitto/certs/openssl-san.cnf

# Generate CA + server cert using the SAN config
sudo bash ./infra/mosquitto/gen-server-cert.sh --config /etc/mosquitto/certs/openssl-san.cnf
```

CA file for devices: `/etc/mosquitto/certs/ca.crt` (copy its PEM contents into your ESP firmware).

## 4) Create Mosquitto Password File (Client Authentication)

```bash
sudo mosquitto_passwd -c /etc/mosquitto/passwd espclient
# Prompted for password

# (Recommended) create a user for mitmproxy addon too:
sudo mosquitto_passwd /etc/mosquitto/passwd mitmclient

sudo chown mosq:mosq /etc/mosquitto/passwd
sudo chmod 600 /etc/mosquitto/passwd
```

Tip: For quick testing only, `mosquitto_sub` can skip hostname verification with `--insecure`, but the proper fix is issuing the server cert with a SAN that matches the host you connect to (e.g. `localhost` or your DNS name).

## 5) Configure Secure Listener (TLS + Auth)

Use the provided infra files

```bash
# Copy ready-to-use configs
sudo cp ./infra/mosquitto/secure.conf /etc/mosquitto/conf.d/secure.conf
sudo cp ./infra/mosquitto/acl /etc/mosquitto/acl

# Ensure mosquitto runs as custom user mosq with sane pre-start steps
sudo mkdir -p /etc/systemd/system/mosquitto.service.d
sudo cp ./infra/mosquitto/override.conf /etc/systemd/system/mosquitto.service.d/override.conf
sudo systemctl daemon-reload
```

Reload and check logs:

```bash
sudo systemctl restart mosquitto
sudo journalctl -u mosquitto -f
```

 

If you see errors like:

```
Error: Unable to open log file /var/log/mosquitto/mosquitto.log for writing.
Error: Unable to load server key file "/etc/mosquitto/certs/server.key". Check keyfile.
```

Fix them:

1. Ensure the runtime user matches file ownership. Using custom user `mosq` everywhere is recommended; all certs, key, CA, passwd and log directory must be owned by `mosq:mosq` with restrictive permissions on the private key.
  - To confirm custom user is applied:
    ```bash
    sudo systemctl edit mosquitto
    # In the editor ensure:
    [Service]
    User=mosq
    Group=mosq
    ```
    Then reload:
    ```bash
    sudo systemctl daemon-reload
    ```
  - Or revert the line in `secure.conf` to `user mosquitto` and adjust ownership:
    ```bash
    sudo chown mosquitto:mosquitto /etc/mosquitto/certs/server.key /etc/mosquitto/certs/server.crt /etc/mosquitto/certs/ca.crt
    sudo chown mosquitto:mosquitto /etc/mosquitto/passwd
    ```
2. Create log directory and file with correct ownership (only if using file logging):
  ```bash
  sudo mkdir -p /var/log/mosquitto
  sudo touch /var/log/mosquitto/mosquitto.log
  sudo chown mosq:mosq /var/log/mosquitto/mosquitto.log /var/log/mosquitto
  sudo chmod 640 /var/log/mosquitto/mosquitto.log
  # (Use mosquitto:mosquitto if you reverted user.)
  ```
3. Restart again:
  ```bash
  sudo systemctl restart mosquitto
  sudo journalctl -u mosquitto -f
  ```

Optional: Instead of a file log, you can remove any `log_dest file` directive and rely on syslog (`log_dest syslog`) to avoid managing permissions.

## 6a) Troubleshooting "Protocol error" on TLS connect

If `mosquitto_sub` reports `Error: Protocol error` on a TLS listener:

Likely causes:
- Wrong port (connecting TLS client to a plain listener or vice versa).
- Certificate/key unreadable by runtime user (results in the broker starting without TLS, or failing; verify service status).
- Client missing `--cafile` or using an incorrect CA (handshake aborts early).
- Corrupted or wrong format key/certificate (must be PEM, not DER).
- Mixed permissions (server.key not readable by the broker user).

Diagnostics:
```bash
sudo systemctl status mosquitto
sudo journalctl -u mosquitto | tail -n 30
openssl s_client -connect localhost:8883 -servername localhost -CAfile /etc/mosquitto/certs/ca.crt -quiet
mosquitto_sub -d -h localhost -p 8883 --cafile /etc/mosquitto/certs/ca.crt -u espclient -P 'YOURPASSWORD' -t test/topic
```

Check that `openssl s_client` ends with `Verify return code: 0 (ok)`.

Permissions sanity:
```bash
ls -l /etc/mosquitto/certs
ls -l /etc/mosquitto/passwd
```

Expected:
- `server.key` : `-rw------- mosq mosq`
- `server.crt` / `ca.crt` : `-rw-r--r-- mosq mosq`
- `passwd` : `-rw------- mosq mosq`

If the broker runs as default user instead, adjust ownership to `mosquitto:mosquitto` accordingly.

Regenerate a broken key/cert pair (if needed):
```bash
cd /etc/mosquitto/certs
sudo openssl genrsa -out server.key 2048
sudo openssl req -new -key server.key -out server.csr
sudo openssl x509 -req -in server.csr -CA ca.crt -CAkey ca.key -CAcreateserial -out server.crt -days 3650
sudo rm server.csr
sudo chown mosq:mosq server.key server.crt
sudo chmod 600 server.key
sudo chmod 644 server.crt
sudo systemctl restart mosquitto
```

## 6) Firewall (iptables)

```bash
# Allow TLS MQTT 8883 (any source)
sudo iptables -A INPUT -p tcp --dport 8883 -j ACCEPT

# Optional: restrict to LAN instead of anywhere
# sudo iptables -A INPUT -p tcp -s 192.168.0.0/16 --dport 8883 -j ACCEPT

# Persist rules across reboots (Debian/Ubuntu)
sudo apt install -y iptables-persistent
sudo netfilter-persistent save
```

Allow entire private networks (RFC1918) only:

```bash
# 10.0.0.0/8
sudo iptables -A INPUT -p tcp -s 10.0.0.0/8 --dport 8883 -j ACCEPT
# 172.16.0.0/12
sudo iptables -A INPUT -p tcp -s 172.16.0.0/12 --dport 8883 -j ACCEPT
# 192.168.0.0/16
sudo iptables -A INPUT -p tcp -s 192.168.0.0/16 --dport 8883 -j ACCEPT

# Remove any broad allow added earlier if present
# sudo iptables -D INPUT -p tcp --dport 8883 -j ACCEPT || true

sudo netfilter-persistent save
```

Tip (TLS SAN when you don’t have a domain or stable public IP):
- Include entries you actually use: `DNS:localhost`, `IP:127.0.0.1`, your LAN IP `IP:<LAN_IP>`.
- For outside access without a domain, either use a free DDNS (DuckDNS, No-IP, Dynu) and put `DNS:<your-ddns>` in SAN, or connect by your current public IP and include that `IP:<PUBLIC_IP>` in SAN (you’ll need to reissue the cert when it changes). For quick tests, clients like mosquitto_sub can use `--insecure` to skip hostname verification.

## 7) Test the Broker

Terminal A (subscribe):

```bash
mosquitto_sub -v \
  -h localhost \
  -p 8883 \
  --cafile /etc/mosquitto/certs/ca.crt \
  -u espclient \
  -P "12_espclient_34" \
  -t "garage/topic"
```

Terminal B (publish):

```bash
mosquitto_pub \
  -h localhost \
  -p 8883 \
  --cafile /etc/mosquitto/certs/ca.crt \
  -u espclient \
  -P "12_espclient_34" \
  -t "garage/cmd" \
  -m "OPEN"
```

Expected output in terminal A:

```
test/topic Hello MQTT!
```

## 11) Final Checklist

- User and ownership:
  - Runtime user `mosq` exists: `id mosq`
  - Certs and passwd owned by `mosq:mosq`: `ls -l /etc/mosquitto/certs /etc/mosquitto/passwd`
  - Permissions: `server.key` 600; `server.crt` and `ca.crt` 644; `passwd` 600
- TLS certificate:
  - SAN includes what you connect to: `localhost`, `<LAN_IP>`, and optionally `<YOUR_DDNS_OR_HOSTNAME>`
  - Verify: `openssl x509 -in /etc/mosquitto/certs/server.crt -noout -text | sed -n '/Subject Alternative Name/,+1p'`
  - Handshake: `openssl s_client -connect localhost:8883 -servername localhost -CAfile /etc/mosquitto/certs/ca.crt -quiet` should end with `Verify return code: 0 (ok)`
- Mosquitto config:
  - `per_listener_settings true` appears before any security lines
  - Listener 8883 has `cafile/certfile/keyfile/tls_version`
  - Security after listener: `user mosq`, `allow_anonymous false`, `password_file ...`, optional `acl_file`
- Auth:
  - Password set: `sudo mosquitto_passwd -b /etc/mosquitto/passwd espclient 'YOURPASSWORD'`
  - Optional ACL grants topics to `espclient`
- Firewall:
  - Open TCP 8883 via iptables (optionally restrict to LAN or RFC1918)

After any change:
```bash
sudo systemctl restart mosquitto
sudo journalctl -u mosquitto -f
```

## 8) ESP8266 Compatibility

Configure firmware with:
- host: broker IP (or hostname)
- port: 8883
- TLS: enabled
- username/password: `espclient` and its password
- CA certificate: paste contents of `/etc/mosquitto/certs/ca.crt` into the firmware’s CA array (PEM)

Topics (suggested):
- Commands: `garage/cmd` (payload: `open`)
- Status: `garage/status` (payloads: `idle`, `opening`, `opened`, `closed`, etc.)

## 9) Optional Hardening

In `/etc/mosquitto/conf.d/secure.conf` you can add:

```
message_size_limit 10240
max_connections 20
```

Disable unencrypted MQTT:

```bash
sudo nano /etc/mosquitto/mosquitto.conf
# add: port 0
sudo systemctl restart mosquitto
```

## 10) Allowing mitmproxy access to manage users (optional)

Clients do not need to read `/etc/mosquitto/passwd`. If you want the `mitmproxy` system user to manage users, prefer `sudoers` for the `mosquitto_passwd` tool:

```bash
sudo visudo -f /etc/sudoers.d/mitmproxy-mosquitto
```

Add:

```
mitmproxy ALL=(root) NOPASSWD: /usr/bin/mosquitto_passwd *
```

If you must grant read access to `/etc/mosquitto/passwd` (not recommended):

```bash
sudo chgrp mitmproxy /etc/mosquitto/passwd
sudo chmod 640 /etc/mosquitto/passwd
```

Security note: prefer not sharing the password file; use per-client credentials and keep file mode `600` owned by `mosq:mosq`.

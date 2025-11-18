# Secure MQTT Broker Setup on Ubuntu (Mosquitto + TLS + Auth + UFW)

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

Run Mosquitto as a locked-down system user:

```bash
sudo adduser --system --no-create-home --disabled-password mosq
```

## 3) Create Secure TLS Certificates (Self-signed CA)

Self-signed private CA is recommended for local networks and IoT. You will embed the CA certificate in the ESP firmware.

```bash
sudo mkdir -p /etc/mosquitto/certs
cd /etc/mosquitto/certs

# Private CA
sudo openssl genrsa -out ca.key 4096
sudo openssl req -new -x509 -days 3650 -key ca.key -out ca.crt

# Broker key + CSR
sudo openssl genrsa -out server.key 2048
sudo openssl req -new -key server.key -out server.csr

# Sign server certificate with your CA
sudo openssl x509 -req -in server.csr -CA ca.crt -CAkey ca.key \
    -CAcreateserial -out server.crt -days 3650

# Cleanup
sudo rm server.csr

# Ownership & permissions
sudo chown mosq:mosq server.key server.crt ca.crt
sudo chmod 600 server.key
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

## 5) Configure Secure Listener (TLS + Auth)

```bash
sudo nano /etc/mosquitto/conf.d/secure.conf
```

Paste:

```
# Disable anonymous access
allow_anonymous false

# Username/password auth
password_file /etc/mosquitto/passwd

# TLS MQTT listener (port 8883)
listener 8883
cafile /etc/mosquitto/certs/ca.crt
certfile /etc/mosquitto/certs/server.crt
keyfile /etc/mosquitto/certs/server.key

tls_version tlsv1.2

# Run under the dedicated user
user mosq

# Per-listener configs
per_listener_settings true
```

Reload and check logs:

```bash
sudo systemctl restart mosquitto
sudo journalctl -u mosquitto -f
```

## 6) Secure Firewall (UFW)

```bash
sudo ufw enable
sudo ufw allow ssh
sudo ufw allow 8883/tcp
# Optional: LAN-only
# sudo ufw allow from 192.168.0.0/16 to any port 8883 proto tcp
sudo ufw status verbose
```

## 7) Test the Broker

Terminal A (subscribe):

```bash
mosquitto_sub -v \
  -h localhost \
  -p 8883 \
  --cafile /etc/mosquitto/certs/ca.crt \
  -u espclient \
  -P "YOURPASSWORD" \
  -t "test/topic"
```

Terminal B (publish):

```bash
mosquitto_pub \
  -h localhost \
  -p 8883 \
  --cafile /etc/mosquitto/certs/ca.crt \
  -u espclient \
  -P "YOURPASSWORD" \
  -t "test/topic" \
  -m "Hello MQTT!"
```

Expected output in terminal A:

```
test/topic Hello MQTT!
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

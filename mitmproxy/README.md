# mitmproxy addon (MQTT trigger)

This addon publishes an MQTT command when a configured trigger pattern is observed in the intercepted flows.

- TLS MQTT on port 8883 with username/password auth
- Configurable trigger via environment variables (SNI, CONNECT host:port, URL substring)
- Cooldown to avoid spamming duplicate commands

## Configure

Create `/etc/blaulicht/mqtt.env`:

```
MQTT_HOST=127.0.0.1
MQTT_PORT=8883
MQTT_USER=mitmclient
MQTT_PASS=REPLACE_WITH_STRONG_PASSWORD
MQTT_CAFILE=/etc/mosquitto/certs/ca.crt
GARAGE_CMD_TOPIC=garage/cmd
GARAGE_CMD_PAYLOAD=open
# Optional trigger configs (pick one or more)
#TRIGGER_SNI=
#TRIGGER_URL_CONTAINS=
#TRIGGER_CONNECT_HOST=
#TRIGGER_CONNECT_PORT=
PUBLISH_COOLDOWN_SEC=10
```

The systemd unit `infra/systemd/mitmproxy.service` includes:

```
EnvironmentFile=-/etc/blaulicht/mqtt.env
```

so the addon can read these variables at start.

## Install dependencies

Use a venv under `/opt/blaulicht/mitmproxy` and install:

```
pip install -r mitmproxy/requirements.txt
```

## Run

With systemd (recommended):

```
sudo systemctl enable --now mitmproxy.service
journalctl -u mitmproxy -f
```

Manual for testing:

```
mitmdump --set confdir=/var/lib/mitmproxy --mode socks5 --listen-host 0.0.0.0 --listen-port 8281 \
  -s /opt/blaulicht/mitmproxy/garage_trigger.py
```

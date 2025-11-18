"""mitmproxy addon: Publish MQTT command when a defined trigger pattern matches.

- Uses TLS-secured MQTT (port 8883) to publish a garage command, e.g. "open".
- Trigger conditions are configurable via environment variables.
  Common choices: match TLS SNI, CONNECT host:port, or URL substring.

Environment variables (with defaults):
  MQTT_HOST=127.0.0.1
  MQTT_PORT=8883
  MQTT_USER=mitmclient   (required)
  MQTT_PASS=...          (required)
  MQTT_CAFILE=/etc/mosquitto/certs/ca.crt
  MQTT_CLIENT_ID=mitmproxy-garage
  GARAGE_CMD_TOPIC=garage/cmd
  GARAGE_CMD_PAYLOAD=open
  TRIGGER_SNI=           (optional exact SNI match)
  TRIGGER_URL_CONTAINS=  (optional substring match on request URL)
  TRIGGER_CONNECT_HOST=  (optional exact CONNECT host match)
  TRIGGER_CONNECT_PORT=  (optional integer port; pairs with host if set)
  PUBLISH_COOLDOWN_SEC=10  (avoid spamming repeated publishes)

Run example:
    mitmdump --mode socks5 --listen-host 0.0.0.0 --listen-port 8281 -s garage_trigger.py
"""

import os
import time
from typing import Optional
from mitmproxy import ctx
import ssl

try:
    import paho.mqtt.client as mqtt
except Exception:  # Keep addon importable; will log error at runtime if missing
    mqtt = None


MAX_BODY_LOG = 1024  # logging cap; not used for decisions by default


class MqttPublisher:
    def __init__(self):
        self.host = os.getenv("MQTT_HOST", "127.0.0.1")
        self.port = int(os.getenv("MQTT_PORT", "8883"))
        self.user = os.getenv("MQTT_USER")
        self.password = os.getenv("MQTT_PASS")
        self.cafile = os.getenv("MQTT_CAFILE", "/etc/mosquitto/certs/ca.crt")
        self.client_id = os.getenv("MQTT_CLIENT_ID", "mitmproxy-garage")

        self.topic = os.getenv("GARAGE_CMD_TOPIC", "garage/cmd")
        self.payload = os.getenv("GARAGE_CMD_PAYLOAD", "open")

        self.cooldown = int(os.getenv("PUBLISH_COOLDOWN_SEC", "10"))
        self._last_publish = 0.0

        self.enabled = True
        if mqtt is None:
            ctx.log.warn("[mqtt] paho-mqtt not installed; MQTT disabled")
            self.enabled = False
            return
        if not (self.user and self.password):
            ctx.log.warn("[mqtt] MQTT_USER/MQTT_PASS not set; MQTT disabled")
            self.enabled = False
            return

        self.client = mqtt.Client(client_id=self.client_id, clean_session=True)
        self.client.username_pw_set(self.user, self.password)
        self.client.tls_set(ca_certs=self.cafile, certfile=None, keyfile=None, tls_version=ssl.PROTOCOL_TLSv1_2)
        self.client.tls_insecure_set(False)
        self.client.on_connect = self._on_connect
        self.client.on_disconnect = self._on_disconnect

        try:
            self.client.connect(self.host, self.port, keepalive=30)
            # Start networking thread
            self.client.loop_start()
            ctx.log.info(f"[mqtt] Connected to {self.host}:{self.port} topic={self.topic}")
        except Exception as e:
            ctx.log.warn(f"[mqtt] Initial connect failed: {e}")
            self.enabled = False

    def _on_connect(self, client, userdata, flags, rc):
        if rc == 0:
            ctx.log.info("[mqtt] Connected (rc=0)")
        else:
            ctx.log.warn(f"[mqtt] Connect returned rc={rc}")

    def _on_disconnect(self, client, userdata, rc):
        ctx.log.warn(f"[mqtt] Disconnected rc={rc}")

    def publish_once(self):
        if not self.enabled:
            return
        now = time.time()
        if now - self._last_publish < self.cooldown:
            ctx.log.info("[mqtt] Cooldown active; skipping publish")
            return
        try:
            r = self.client.publish(self.topic, self.payload, qos=1, retain=False)
            r.wait_for_publish()
            if r.rc == 0:
                ctx.log.info(f"[mqtt] Published cmd to {self.topic}: {self.payload}")
                self._last_publish = now
            else:
                ctx.log.warn(f"[mqtt] Publish rc={r.rc}")
        except Exception as e:
            ctx.log.warn(f"[mqtt] Publish failed: {e}")


class GarageTrigger:
    def __init__(self):
        self.mqtt = MqttPublisher()
        # Trigger config
        self.trigger_sni = os.getenv("TRIGGER_SNI")
        self.trigger_url_contains = os.getenv("TRIGGER_URL_CONTAINS")
        self.trigger_connect_host = os.getenv("TRIGGER_CONNECT_HOST")
        self.trigger_connect_port = os.getenv("TRIGGER_CONNECT_PORT")
        self.trigger_connect_port = int(self.trigger_connect_port) if self.trigger_connect_port else None
        ctx.log.info("[garage] MQTT trigger addon initialized")

    def _maybe_publish(self, reason: str):
        ctx.log.info(f"[garage] Trigger matched: {reason}")
        self.mqtt.publish_once()

    def request(self, flow):  # HTTP request visible when TLS intercepted
        try:
            url = flow.request.pretty_url
            if self.trigger_url_contains and self.trigger_url_contains in url:
                self._maybe_publish(f"url contains '{self.trigger_url_contains}'")
        except Exception as ex:
            ctx.log.warn(f"[garage] request() error: {ex}")

    def http_connect(self, flow):  # HTTPS CONNECT visibility without TLS intercept
        try:
            host = getattr(flow.request, "host", "?")
            port = getattr(flow.request, "port", 0)
            if self.trigger_connect_host and host == self.trigger_connect_host:
                if self.trigger_connect_port is None or self.trigger_connect_port == port:
                    self._maybe_publish(f"connect {host}:{port}")
        except Exception as ex:
            ctx.log.warn(f"[garage] http_connect error: {ex}")

    def tls_clienthello(self, data):  # SNI match without full MITM
        try:
            client_hello = getattr(data, "client_hello", None)
            sni = getattr(client_hello, "sni", None)
            if self.trigger_sni and sni == self.trigger_sni:
                self._maybe_publish(f"sni={sni}")
        except Exception as ex:
            ctx.log.warn(f"[garage] tls_clienthello error: {ex}")

    def error(self, flow):
        try:
            if flow.error:
                ctx.log.warn(f"[garage] flow error: {flow.error}")
        except Exception as ex:
            ctx.log.warn(f"[garage] error() error: {ex}")


addons = [GarageTrigger()]

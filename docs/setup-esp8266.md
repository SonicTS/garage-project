# ESP8266 Setup (MQTT)

The firmware (PlatformIO, ESP8266 RTOS SDK) connects to Wi‑Fi and to your Mosquitto broker over TLS, subscribes to `garage/cmd`, and publishes status on `garage/status`. The MQTT client is implemented in `esp8266/Garage-Controller/lib/mqtt` using mbedTLS.

## Configure Wi‑Fi + MQTT

Edit the configuration via `config_store` defaults or your own UI. Key fields (structs in `lib/config_store/include/config_store.h`):

- `wifi.ssid`, `wifi.password`
- `mqtt.broker` (e.g. `192.168.1.10`), `mqtt.port = 8883`
- `mqtt.client_id` (e.g. `garage-esp`)
- `mqtt.username`, `mqtt.password` (created with `mosquitto_passwd`)
- `mqtt.base_topic` (e.g. `garage/door1`) – used by the helper publisher
- `mqtt.use_tls = 1`

Embed your CA certificate (PEM) in `lib/mqtt/src/mqtt.c` by replacing the placeholder inside `mqtt_ca_pem[]` with the content of `/etc/mosquitto/certs/ca.crt`:

```
static const char mqtt_ca_pem[] =
"-----BEGIN CERTIFICATE-----\n"
"... paste ca.crt PEM here ...\n"
"-----END CERTIFICATE-----\n";
```

Topics:
- Commands (subscribe): `garage/cmd` (payload: `open`)
- Status (publish): `garage/status` (payloads like `idle`, `opening`, `opened`, `closed`)

The helper `mqtt_client_publish_status(subtopic, payload)` publishes to `${base_topic}/${subtopic}`; for a flat topic like `garage/status`, set `base_topic="garage"` and subtopic to `status`.

## Build & Flash (PlatformIO)

1) Open `esp8266/Garage-Controller` in VS Code with the PlatformIO extension.
2) Update configuration as above and paste the CA certificate.
3) Build (PlatformIO: Build) and Upload (PlatformIO: Upload) to your board.
4) Monitor serial output to confirm Wi‑Fi + MQTT connection and command handling.

## Notes

- Ensure RTC/peripherals meet your relay board’s logic level and activation polarity.
- Implement relay cooldown in your app logic to avoid rapid toggles.
- For mTLS, you could extend `mqtt.c` to load a client cert/key if desired.

# ESP8266 Setup

This is an Arduino-style sketch targeting a common ESP8266 dev board (e.g. NodeMCU). The device connects to Wi‑Fi and periodically polls the garage-controller’s `/garage/poll` endpoint. When it receives `{"cmd":"open"}`, it pulses a relay GPIO.

## What the firmware should do

- Connect to your 2.4 GHz Wi‑Fi network.
- Every few seconds, GET `https://<DOMAIN_OR_DDNS>/garage/poll?device=<DEVICE_ID>&token=<TOKEN>`.
- Parse JSON response:
  - If `{ "cmd": "open" }` → pulse a GPIO (e.g., D1) for ~500 ms, then wait for the command to clear.
  - Else `{ "cmd": "idle" }` → do nothing and continue polling.

## TODOs in the sketch

- [ ] Wi‑Fi SSID and password
- [ ] `<DOMAIN_OR_DDNS>`
- [ ] `DEVICE_ID` and `TOKEN`
- [ ] Chosen GPIO pin for relay
- [ ] TLS validation method (fingerprint/CA). Prototype may use `setInsecure()` but that’s not recommended for production.

## Flashing Steps

1. Install Arduino IDE or use PlatformIO.
2. Board Manager: install “ESP8266 by ESP8266 Community”. Select your board (e.g., NodeMCU 1.0).
3. Open `esp8266/firmware.ino` from this repo.
4. Fill in the TODOs (Wi‑Fi, device ID/token, server host, etc.).
5. Connect the board over USB; choose the correct COM/serial port.
6. Compile and Upload.

## Notes

- GPIO driving the relay should match your relay board’s logic (HIGH to activate vs. LOW to activate). Adjust accordingly.
- Add a small debounce or cooldown to avoid rapid repeated openings.
- Consider persisting minimal state or adding a watchdog for reliability.

# ESP8266 Firmware

Target board: NodeMCU / generic ESP8266 development board.

## Purpose

Poll the garage-controller `/garage/poll` endpoint and pulse a relay when instructed to open.

## Flashing (Arduino IDE)

1. Install "ESP8266 by ESP8266 Community" via Board Manager.
2. Select your board (e.g., NodeMCU 1.0) and correct COM port.
3. Open `firmware.ino`.
4. Fill in Wi‑Fi credentials, server host, device ID, and token.
5. Upload.

## Relay Wiring (Text Only)

- Use a transistor/relay module appropriate for your door opener’s control circuit.
- GPIO (e.g., D1) → Relay IN; 3V3 / GND to module power.
- The relay NO (Normally Open) contacts go across the garage opener’s push-button terminals. The pulse simulates a button press.

## Notes / TODOs

- Implement proper TLS certificate validation (fingerprint or CA bundle).
- Add cooldown to avoid rapid repeated openings.
- Consider watchdog reset for reliability.

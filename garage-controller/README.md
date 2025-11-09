# garage-controller

Small FastAPI service that stores pending "open" commands for ESP8266 devices polling over HTTPS.

## Endpoints

- `POST /trigger` (local-only)
  - Accepts JSON: `{"device": "brother"}`
  - Queues a pending command `{"cmd": "open"}` for that device.
  - Intended to be called exclusively by the mitmproxy addon via `http://127.0.0.1:5001/trigger`.
- `GET /poll?device=<id>&token=<token>`
  - Verifies token against config.
  - Returns `{"cmd": "open"}` if a pending command exists (and clears it), else `{"cmd": "idle"}`.

## Interaction Flow

1. mitmproxy addon detects HTTP(S) pattern → POST /trigger.
2. Controller stores command in memory.
3. ESP8266 polls `/garage/poll` and receives `open` → pulses relay.
4. Subsequent polls return `idle` until next trigger.

## Configuration

`config.example.yml`:
```yaml
devices:
  brother:
    token: "CHANGE_ME_TO_A_LONG_RANDOM_STRING"
server:
  trigger_port: 5001
  poll_port: 5002
```

Copy to `config.yml` and change tokens to long random values.

## Development Run

```bash
python3 -m venv venv
source venv/bin/activate  # On Windows: venv\Scripts\activate
pip install -r requirements.txt
python app.py
```

The app will start on the configured `poll_port` (default 5002). Both `/trigger` and `/poll` are served by the same process in this scaffold.

## TODOs / Enhancements

- Enforce localhost-only access to `/trigger`.
- Add command debounce / TTL.
- Persistent storage (SQLite, etc.) instead of in-memory dict.
- Structured logging / metrics.
- Multiple command types (open, close, status) if hardware supports.

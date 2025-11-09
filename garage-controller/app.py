"""Garage Controller Service (FastAPI skeleton)

Provides two endpoints:
    POST /trigger  (local-only) - Set a pending command for a device, e.g. open the garage.
    GET  /poll     (public)     - Device polls with its token; receives command or idle.

Configuration loaded from `config.yml` located in the same directory by default.

Example config.yml structure:
    devices:
      brother:
        token: "CHANGE_ME"
    server:
      trigger_port: 5001
      poll_port: 5002

TODOs:
    - Enforce localhost-only access to /trigger (check client IP).
    - Add debouncing / cooldown / TTL for commands.
    - Replace in-memory state with a persistent store if desired.
    - Add logging and structured output.
    - Consider returning door state or diagnostics.
"""

from typing import Dict, Any
from fastapi import FastAPI, Request, HTTPException
from fastapi.responses import JSONResponse
import yaml
import os


CONFIG_FILENAME = "config.yml"
BASE_DIR = os.path.dirname(os.path.abspath(__file__))
CONFIG_PATH = os.path.join(BASE_DIR, CONFIG_FILENAME)

app = FastAPI(title="Garage Controller", description="Manages garage open commands for ESP8266 clients.")


def load_config(path: str = CONFIG_PATH) -> Dict[str, Any]:
    """Load YAML configuration.

    Raises FileNotFoundError if missing. In production you might want to fail closed.
    """
    with open(path, "r", encoding="utf-8") as f:
        return yaml.safe_load(f) or {}


try:
    CONFIG = load_config()
except FileNotFoundError:
    # For development scaffolding, fall back to an empty config.
    CONFIG = {"devices": {}}  # NOTE: In production, fail closed if config missing.

# In-memory pending commands: {device_name: {"cmd": "open"}}
PENDING: Dict[str, Dict[str, str]] = {}


def verify_device(device: str, token: str) -> bool:
    """Return True if device exists and token matches config."""
    devices = CONFIG.get("devices", {})
    entry = devices.get(device)
    if not entry:
        return False
    return token == entry.get("token")


@app.post("/trigger")
async def trigger(request: Request):
    """Set a pending 'open' command for a device.

    Expected JSON body: {"device": "brother"}

    Notes:
        - Restrict to localhost only: check request.client.host == '127.0.0.1'.
        - Validate device existence before setting command.
        - Maybe support different command types in future.
    """
    # NOTE: Enforce localhost restriction here if desired.
    data = await request.json()
    device = data.get("device")
    if not device:
        raise HTTPException(status_code=400, detail="Missing 'device' field")
    if device not in CONFIG.get("devices", {}):
        raise HTTPException(status_code=404, detail="Unknown device")

    # Set pending command.
    PENDING[device] = {"cmd": "open"}
    return JSONResponse({"status": "queued", "device": device})


@app.get("/poll")
async def poll(device: str, token: str):
    """Device polls for commands.

    Query params: device, token
    Returns JSON {"cmd": "open"} or {"cmd": "idle"}.

    TODO:
        - Add rate limiting or minimal sleep to prevent hammering.
        - Debounce: ensure command not repeated within too-short interval.
        - Possibly return additional metadata (timestamp, version).
    """
    if not verify_device(device, token):
        raise HTTPException(status_code=403, detail="Invalid device or token")

    pending = PENDING.pop(device, None)
    if pending:
        return JSONResponse(pending)
    return JSONResponse({"cmd": "idle"})


def main():  # Convenience entrypoint for uvicorn programmatic run.
    import uvicorn
    # Ports come from config, or fallback defaults.
    server_cfg = CONFIG.get("server", {})
    poll_port = server_cfg.get("poll_port", 5002)
    # Typically /trigger might run separately; here we just run a single app hosting both endpoints.
    # For production you might host behind nginx.
    uvicorn.run(app, host="0.0.0.0", port=poll_port, log_level="info")


if __name__ == "__main__":
    # Running `python app.py` will start uvicorn using config-defined poll port.
    main()

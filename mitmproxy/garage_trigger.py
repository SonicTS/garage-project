"""mitmproxy addon that watches flows and triggers the garage-controller.

Skeleton implementation: fill in the actual trigger condition logic.

Usage (example):
    mitmdump --mode socks5 --listen-host 0.0.0.0 --listen-port 1080 -s garage_trigger.py

TODO:
- Define trigger condition (URL pattern, headers, body substring, etc.).
- Consider rate limiting / debouncing multiple triggers.
- Add logging (structured?) if needed.
"""

from typing import Any, Dict
import requests  # mitmproxy runs in CPython; requests is acceptable for simplicity.

# NOTE: mitmproxy provides the Flow object with request/response details.


class GarageTrigger:
    """Addon that inspects HTTP requests and POSTs to a local trigger endpoint.

    Args:
        trigger_device: Device name to include in JSON payload (e.g., 'brother').
        trigger_url: Local URL for garage-controller /trigger endpoint.
        condition_hint: Human-readable description of what should match (for doc purposes).
    """

    def __init__(self, trigger_device: str = "brother", trigger_url: str = "http://127.0.0.1:5001/trigger", condition_hint: str = "TODO: describe condition"):
        self.trigger_device = trigger_device
        self.trigger_url = trigger_url
        self.condition_hint = condition_hint
    # Initialize placeholder for rate limiting state if needed.

    def request(self, flow):  # mitmproxy hook
        """Called when a client request has been received.

        Use flow.request to inspect:
            - flow.request.pretty_url
            - flow.request.method
            - flow.request.headers
            - flow.request.get_text() (body as string)

        TODO: Replace placeholder condition with real logic.
        """
        try:
            _url = flow.request.pretty_url
            _method = flow.request.method

            # Define the actual trigger condition here (URL match, method, header or body snippet).
            trigger_condition = False

            # Example placeholder logic:
            # if "example_endpoint" in url and method == "POST":
            #     trigger_condition = True

            if trigger_condition:
                payload: Dict[str, Any] = {"device": self.trigger_device}
                # Optional: include additional metadata from the flow if needed.
                self._fire_trigger(payload)
        except Exception as ex:  # Broad catch to prevent crashing addon; refine as needed.
            # Simple print; replace with logging framework if desired.
            print(f"[garage_trigger] Exception during request inspection: {ex}")

    def _fire_trigger(self, payload: Dict[str, Any]) -> None:
        """
        POST to the local trigger URL with JSON payload.

        Consider rate limiting and error backoff in future enhancements.
        """
        try:
            resp = requests.post(self.trigger_url, json=payload, timeout=2)
            if resp.status_code != 200:
                print(f"[garage_trigger] Trigger POST non-200: {resp.status_code} body={resp.text}")
            else:
                print(f"[garage_trigger] Triggered for device={payload.get('device')}")
        except requests.RequestException as rex:
            print(f"[garage_trigger] Request error posting trigger: {rex}")


# Export list for mitmproxy to recognize addons
addons = [GarageTrigger()]  # Pass custom args if needed: GarageTrigger(trigger_device="brother")

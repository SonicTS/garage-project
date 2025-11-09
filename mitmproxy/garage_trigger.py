"""mitmproxy addon: log HTTP(S) traffic for analysis, including CONNECT/TLS hints.

Use this first to observe the real requests/responses and decide on a trigger pattern later.

Run example:
    mitmdump --mode socks5 --listen-host 0.0.0.0 --listen-port 8281 -s garage_trigger.py

Notes:
- Bodies are truncated to avoid huge logs. Adjust MAX_BODY_LOG if needed.
- Remove/adjust header/body logging if the data may include secrets.
"""

from typing import Optional
from mitmproxy import ctx

# NOTE: mitmproxy provides the Flow object with request/response details.

MAX_BODY_LOG = 2048  # bytes/characters to log from bodies


class GarageTrigger:
    """Logs request/response details for every flow.

    Later you can convert this to detect a pattern and call a local trigger.
    """

    def __init__(self):
        ctx.log.info("[garage_logger] Initialized. Logging all flows.")

    def request(self, flow):  # mitmproxy hook
        """Log inbound client request details."""
        try:
            url = flow.request.pretty_url
            method = flow.request.method
            ctx.log.info(f"[REQ] {method} {url}")

            # Headers
            for k, v in flow.request.headers.items():
                ctx.log.info(f"[REQ][H] {k}: {v}")

            # Body (truncate for safety)
            body_text: Optional[str] = None
            try:
                body_text = flow.request.get_text()
            except Exception:
                body_text = None
            if body_text:
                snippet = body_text[:MAX_BODY_LOG]
                if len(body_text) > MAX_BODY_LOG:
                    snippet += "\n... [truncated]"
                ctx.log.info(f"[REQ][B] {snippet}")
            else:
                # If binary or empty, log size if available
                size = len(flow.request.raw_content) if flow.request.raw_content else 0
                ctx.log.info(f"[REQ][B] <no text body> size={size} bytes")
        except Exception as ex:  # Keep addon resilient
            ctx.log.warn(f"[garage_logger] Error logging request: {ex}")

    def http_connect(self, flow):  # mitmproxy hook for HTTPS CONNECT tunnels
        """Log CONNECT target host:port even if TLS is not intercepted."""
        try:
            host = getattr(flow.request, "host", "?")
            port = getattr(flow.request, "port", "?")
            ctx.log.info(f"[CONNECT] {host}:{port}")
        except Exception as ex:
            ctx.log.warn(f"[garage_logger] Error logging CONNECT: {ex}")

    def tls_clienthello(self, data):  # mitmproxy hook for TLS handshake info
        """Log SNI observed in ClientHello (if provided)."""
        try:
            # data.client_hello.sni is available in recent mitmproxy versions
            client_hello = getattr(data, "client_hello", None)
            sni = getattr(client_hello, "sni", None)
            if sni:
                ctx.log.info(f"[TLS] ClientHello SNI={sni}")
            else:
                ctx.log.info("[TLS] ClientHello (no SNI)")
        except Exception as ex:
            ctx.log.warn(f"[garage_logger] Error logging TLS ClientHello: {ex}")

    def response(self, flow):  # mitmproxy hook
        """Log server response details."""
        try:
            status = flow.response.status_code if flow.response else -1
            ctype = flow.response.headers.get("content-type", "") if flow.response else ""
            size = len(flow.response.raw_content) if flow.response and flow.response.raw_content else 0
            ctx.log.info(f"[RES] {status} {ctype} size={size}")

            body_text: Optional[str] = None
            try:
                body_text = flow.response.get_text()
            except Exception:
                body_text = None
            if body_text:
                snippet = body_text[:MAX_BODY_LOG]
                if len(body_text) > MAX_BODY_LOG:
                    snippet += "\n... [truncated]"
                ctx.log.info(f"[RES][B] {snippet}")
            else:
                ctx.log.info("[RES][B] <no text body or binary>")
        except Exception as ex:
            ctx.log.warn(f"[garage_logger] Error logging response: {ex}")

    def error(self, flow):  # mitmproxy hook
        """Log flow-level errors (network failures, timeouts, etc.)."""
        try:
            if flow.error:
                ctx.log.warn(f"[FLOW-ERROR] {flow.error}")
        except Exception as ex:
            ctx.log.warn(f"[garage_logger] Error in error() hook: {ex}")


# Export list for mitmproxy to recognize addons
addons = [GarageTrigger()]

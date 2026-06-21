"""Server-Sent Events endpoint.

One stream carries all four event types — node_status, job_event,
monitor_cycle, telemetry — because browsers cap concurrent connections per
origin and the console needs several at once. The browser filters client-side.
"""

import json
import queue

from flask import Blueprint, Response, stream_with_context

from server import auth
from server.events import bus

stream = Blueprint("stream", __name__, url_prefix="/api")

# Emit a comment line if idle this long, so proxies do not drop a quiet stream.
KEEPALIVE_S = 15


def format_sse(event: str, data: dict) -> str:
    """Render one SSE frame: an event line, a JSON data line, blank terminator."""
    return f"event: {event}\ndata: {json.dumps(data)}\n\n"


@stream.get("/stream")
@auth.require_auth
def sse():
    subscriber = bus.subscribe()

    @stream_with_context
    def generate():
        try:
            yield ": connected\n\n"
            while True:
                try:
                    message = subscriber.get(timeout=KEEPALIVE_S)
                except queue.Empty:
                    yield ": keepalive\n\n"
                    continue
                yield format_sse(message["type"], message["data"])
        finally:
            bus.unsubscribe(subscriber)

    return Response(generate(), mimetype="text/event-stream")

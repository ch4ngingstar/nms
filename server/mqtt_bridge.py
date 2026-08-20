"""MQTT client thread bridging the broker to the ingest handlers.

The bridge owns the connection and the thread; `ingest` owns the semantics.
Every inbound payload is validated by `protocol.validate` *before* anything
touches the database — malformed input from a probe must never become a row.

`dispatch` is separated from the client so routing is testable with a dict and
no broker. The paho client, connection, and reconnect belong to `MqttBridge`.
"""

import json
import logging

import paho.mqtt.client as mqtt

from protocol.errors import ProtocolError
from protocol.topics import NAMESPACE
from protocol.validate import validate_message
from server import commands, ingest

log = logging.getLogger(__name__)

SUBSCRIPTION = f"{NAMESPACE}/#"

# Topic leaf → handler. `cmd` is absent: the server publishes commands, it does
# not consume them, so a cmd echo is ignored rather than re-ingested.
HANDLERS = {
    "announce": ingest.handle_announce,
    "status": ingest.handle_status,
    "telemetry": ingest.handle_telemetry,
    "monitor": ingest.handle_monitor,
    "result": ingest.handle_result,
}


def topic_leaf(topic: str) -> str:
    return topic.rsplit("/", 1)[-1]


def dispatch(topic: str, payload: bytes) -> None:
    """Validate one inbound message and route it to its handler.

    Assumes a Flask app context is active (the handlers use db.session).
    Anything that fails to parse or validate is logged and dropped.
    """
    handler = HANDLERS.get(topic_leaf(topic))
    if handler is None:
        return
    try:
        message = validate_message(json.loads(payload))
    except (ProtocolError, ValueError) as exc:
        log.warning("dropping invalid message on %s: %s", topic, exc)
        return
    handler(message)


class MqttBridge:
    """Owns the paho client, its thread, and the outbound publish path."""

    def __init__(self, app, host: str, port: int = 1883,
                 username: str | None = None, password: str | None = None,
                 client_id: str = "probe-server"):
        self.app = app
        self.host = host
        self.port = port
        self.client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                                  client_id=client_id)
        if username is not None:
            self.client.username_pw_set(username, password)
        # Reconnect with exponential backoff and jitter.
        self.client.reconnect_delay_set(min_delay=1, max_delay=60)
        self.client.on_connect = self._on_connect
        self.client.on_message = self._on_message

    def _on_connect(self, client, userdata, flags, reason_code, properties=None):
        client.subscribe(SUBSCRIPTION, qos=1)

    def _on_message(self, client, userdata, message):
        with self.app.app_context():
            dispatch(message.topic, message.payload)

    def publish(self, topic: str, payload: bytes) -> None:
        self.client.publish(topic, payload, qos=1)

    def start(self) -> None:
        """Connect, register as the command publisher, and loop on a thread."""
        commands.set_publisher(self.publish)
        self.client.connect(self.host, self.port)
        self.client.loop_start()

    def stop(self) -> None:
        self.client.loop_stop()
        self.client.disconnect()

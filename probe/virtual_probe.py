"""The virtual probe process.

`VirtualProbe` holds the protocol behaviour with an injected publisher, so the
whole job lifecycle — accepted, streamed chunks, done/error, busy rejection,
cancellation — is testable without a broker. `main()` wires it to a real paho
client with a Last Will, which is what makes killing this process demonstrate
node-death detection (scenarios 5 and 6).
"""

import json
import os
import secrets
import time
from pathlib import Path

from probe.jobs import CAPABILITIES, run_command
from protocol.topics import (
    ANNOUNCE_TOPIC, cmd_topic, result_topic, status_topic,
)

CONTROL_ONLY = {"identify", "reboot", "get_config"}
CHUNK_MAX_BYTES = 1024


class VirtualProbe:
    """Protocol behaviour for a server-side probe, publisher injected."""

    def __init__(self, node_id: str, publish, capabilities: list[str] | None = None,
                 clock=time.time, config_path: str | Path | None = None):
        self.node_id = node_id
        self._publish = publish
        self.capabilities = capabilities or list(CAPABILITIES)
        self._clock = clock
        self._config_path = Path(config_path) if config_path else None
        self._busy = False
        self._cancel_job: str | None = None
        self.monitor_config: dict | None = self._load_config()

    # --- message construction ---------------------------------------------

    def _envelope(self, type_: str, data: dict) -> dict:
        return {"v": 1, "type": type_, "node": self.node_id,
                "msg_id": secrets.token_hex(8), "ts": int(self._clock()),
                "data": data}

    def _emit(self, topic: str, type_: str, data: dict) -> None:
        self._publish(topic, json.dumps(self._envelope(type_, data)).encode("utf-8"))

    def _result(self, data: dict) -> None:
        self._emit(result_topic(self.node_id), "result", data)

    def announce(self, *, fw: str = "0.0.0", chip: str = "virtual",
                 mac: str = "00:00:00:00:00:00", free_heap: int = 200000,
                 label: str = "virtual probe") -> None:
        self._emit(ANNOUNCE_TOPIC, "announce",
                   {"label": label, "fw": fw, "chip": chip, "mac": mac,
                    "free_heap": free_heap, "capabilities": self.capabilities})

    def status(self, state: str, **extra) -> None:
        self._emit(status_topic(self.node_id), "status", {"state": state, **extra})

    # --- command handling --------------------------------------------------

    def request_cancel(self, job_id: str) -> None:
        self._cancel_job = job_id

    def handle_cmd(self, message: dict) -> None:
        data = message["data"]
        cmd = data["cmd"]
        job_id = data["job_id"]
        args = data.get("args", {})

        if cmd == "cancel":
            self.request_cancel(job_id)
            return
        if cmd == "set_monitor":
            self._set_monitor(job_id, args)
            return
        if cmd in CONTROL_ONLY:
            self._result({"event": "accepted", "job_id": job_id})
            self._result({"event": "done", "job_id": job_id,
                          "chunks": 0, "results": 0, "duration_ms": 0})
            return

        if self._busy:
            self._result({"event": "error", "job_id": job_id, "code": "busy",
                          "message": "a job is already running"})
            return
        self._run(cmd, job_id, args)

    def _set_monitor(self, job_id: str, args: dict) -> None:
        self.monitor_config = args
        self._save_config(args)
        self._result({"event": "accepted", "job_id": job_id})
        self._result({"event": "done", "job_id": job_id,
                      "chunks": 0, "results": 0, "duration_ms": 0})

    def _run(self, cmd: str, job_id: str, args: dict) -> None:
        self._busy = True
        self._cancel_job = None
        self._result({"event": "accepted", "job_id": job_id})
        started = self._clock()
        seq = 0
        try:
            for chunk in run_command(cmd, args):
                if self._cancel_job == job_id:
                    self._result({"event": "error", "job_id": job_id,
                                  "code": "cancelled",
                                  "message": "cancelled by operator"})
                    return
                self._result({"event": "chunk", "job_id": job_id, "seq": seq,
                              **chunk})
                seq += 1
            duration_ms = int((self._clock() - started) * 1000)
            self._result({"event": "done", "job_id": job_id, "chunks": seq,
                          "results": seq, "duration_ms": duration_ms})
        except ValueError as exc:
            self._result({"event": "error", "job_id": job_id,
                          "code": "unsupported", "message": str(exc)})
        finally:
            self._busy = False

    # --- monitor config persistence ---------------------------------------

    def _load_config(self) -> dict | None:
        if self._config_path and self._config_path.is_file():
            return json.loads(self._config_path.read_text(encoding="utf-8"))
        return None

    def _save_config(self, config: dict) -> None:
        if self._config_path:
            self._config_path.write_text(json.dumps(config), encoding="utf-8")


def main() -> None:  # pragma: no cover - integration entry point
    """Wire the probe to a real broker with a Last Will and loop forever."""
    import paho.mqtt.client as mqtt

    node_id = os.environ.get("NMS_NODE_ID", "probe-server")
    host = os.environ.get("NMS_BROKER_HOST", "localhost")
    port = int(os.environ.get("NMS_BROKER_PORT", "1883"))
    username = os.environ.get("NMS_BROKER_USER", node_id)
    password = os.environ.get("NMS_BROKER_PASS")
    config_path = os.environ.get("NMS_MONITOR_CONFIG", "monitor_config.json")

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=node_id)
    if password is not None:
        client.username_pw_set(username, password)

    probe = VirtualProbe(node_id, publish=lambda t, p: client.publish(t, p, qos=1),
                         config_path=config_path)

    # Last Will: the broker publishes this if the probe dies ungracefully,
    # which is exactly how node-death detection is demonstrated.
    lwt = json.dumps({"v": 1, "type": "status", "node": node_id,
                      "msg_id": secrets.token_hex(8), "ts": int(time.time()),
                      "data": {"state": "offline", "reason": "lwt"}})
    client.will_set(status_topic(node_id), lwt, qos=1)

    def on_connect(cl, userdata, flags, reason_code, properties=None):
        cl.subscribe(cmd_topic(node_id), qos=1)
        probe.announce()
        probe.status("online")

    def on_message(cl, userdata, message):
        probe.handle_cmd(json.loads(message.payload))

    client.on_connect = on_connect
    client.on_message = on_message
    client.reconnect_delay_set(min_delay=1, max_delay=60)
    client.connect(host, port)
    client.loop_forever()


if __name__ == "__main__":  # pragma: no cover
    main()

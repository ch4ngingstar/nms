"""Start the whole stack with one command (spec §12).

Brings up the Mosquitto broker, waits for it to accept connections, then runs
the Flask server (with its MQTT bridge and maintenance thread) and the virtual
probe in one place — so day-to-day work is a single command despite three
processes.

Environment:
  SECRET_KEY            required; the server refuses to start without it
  NMS_BROKER_HOST       default localhost
  NMS_BROKER_PORT       default 1883
  NMS_BROKER_USER/PASS  broker credentials for the server account
"""

import os
import socket
import subprocess
import sys
import threading
import time

# pragma: no cover  (orchestration; exercised by hand, not the unit suite)


def _wait_for_broker(host: str, port: int, timeout_s: float = 30.0) -> None:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        try:
            with socket.create_connection((host, port), timeout=2):
                return
        except OSError:
            time.sleep(0.5)
    raise SystemExit(f"broker at {host}:{port} did not come up in {timeout_s}s")


def main() -> None:
    if not os.environ.get("SECRET_KEY"):
        raise SystemExit("SECRET_KEY is not set; refusing to start (spec §11)")

    host = os.environ.get("NMS_BROKER_HOST", "localhost")
    port = int(os.environ.get("NMS_BROKER_PORT", "1883"))

    print("[run_all] starting broker via docker compose ...")
    subprocess.run(["docker", "compose", "up", "-d", "mosquitto"], check=True)
    _wait_for_broker(host, port)
    print("[run_all] broker is up")

    # Import after env is validated so the factory's SECRET_KEY check is honoured.
    from server import maintenance
    from server.app import create_app
    from server.db import db
    from server.mqtt_bridge import MqttBridge

    app = create_app()
    with app.app_context():
        db.create_all()

    bridge = MqttBridge(app, host, port,
                        username=os.environ.get("NMS_BROKER_USER", "nms-server"),
                        password=os.environ.get("NMS_BROKER_PASS"))
    bridge.start()
    print("[run_all] MQTT bridge connected")

    stop = threading.Event()
    threading.Thread(target=maintenance.run_forever, args=(app, stop),
                     daemon=True).start()

    probe = subprocess.Popen([sys.executable, "-m", "probe.virtual_probe"],
                             env={**os.environ})
    print("[run_all] virtual probe launched")

    try:
        app.run(host="0.0.0.0", port=5000, debug=False, use_reloader=False)
    finally:
        stop.set()
        bridge.stop()
        probe.terminate()


if __name__ == "__main__":
    main()

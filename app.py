import platform
import re
import json
import socket
import sqlite3
import subprocess
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone

from flask import Flask, jsonify, render_template, request
from flask_sqlalchemy import SQLAlchemy


app = Flask(__name__)
app.config["SQLALCHEMY_DATABASE_URI"] = "sqlite:///nms.db"
app.config["SQLALCHEMY_TRACK_MODIFICATIONS"] = False

db = SQLAlchemy(app)

PING_TIMEOUT_SECONDS = 2
POLL_INTERVAL_SECONDS = 5
MAX_WORKERS = 20
ALERT_FAILURE_THRESHOLD = 3
SERVICE_PORTS = [22, 53, 80]
DEVICE_LOG_DB = "network_monitor.db"

poller_started = False


class Device(db.Model):
    __tablename__ = "devices"

    id = db.Column(db.Integer, primary_key=True)
    name = db.Column(db.String(120), nullable=False)
    ip_address = db.Column(db.String(64), nullable=False, unique=True, index=True)
    role = db.Column(db.String(80), nullable=False, default="unknown")

    ping_results = db.relationship(
        "PingResult",
        back_populates="device",
        cascade="all, delete-orphan",
        lazy="dynamic",
    )

    def to_dict(self):
        latest_ping = (
            self.ping_results.order_by(PingResult.checked_at.desc()).first()
        )

        consecutive_failures = get_consecutive_failures(self.id)
        status = "unknown"

        if latest_ping:
            status = latest_ping.status

        if consecutive_failures >= ALERT_FAILURE_THRESHOLD:
            status = "warning"

        return {
            "id": self.id,
            "name": self.name,
            "ip_address": self.ip_address,
            "role": self.role,
            "status": status,
            "consecutive_failures": consecutive_failures,
            "last_latency_ms": latest_ping.latency_ms if latest_ping else None,
            "last_checked_at": latest_ping.checked_at.isoformat() if latest_ping else None,
        }


class PingResult(db.Model):
    __tablename__ = "ping_results"

    id = db.Column(db.Integer, primary_key=True)
    device_id = db.Column(
        db.Integer,
        db.ForeignKey("devices.id", ondelete="CASCADE"),
        nullable=False,
        index=True,
    )
    checked_at = db.Column(
        db.DateTime(timezone=True),
        nullable=False,
        default=lambda: datetime.now(timezone.utc),
        index=True,
    )
    status = db.Column(db.String(20), nullable=False, index=True)
    latency_ms = db.Column(db.Float, nullable=True)
    error = db.Column(db.String(255), nullable=True)

    device = db.relationship("Device", back_populates="ping_results")

    def to_dict(self):
        return {
            "id": self.id,
            "device_id": self.device_id,
            "device_name": self.device.name,
            "checked_at": self.checked_at.isoformat(),
            "status": self.status,
            "latency_ms": self.latency_ms,
            "error": self.error,
        }


def seed_devices():
    """Populate the database once, replacing the old hardcoded DEVICES list."""
    if Device.query.count() > 0:
        return

    default_devices = [
        Device(name="Main Router", ip_address="192.168.1.1", role="router"),
        Device(name="Core DNS", ip_address="8.8.8.8", role="dns"),
        Device(name="Cloudflare DNS", ip_address="1.1.1.1", role="dns"),
        Device(name="Edge Server", ip_address="192.168.1.10", role="server"),
    ]

    db.session.add_all(default_devices)
    db.session.commit()


def init_device_logs_db():
    connection = sqlite3.connect(DEVICE_LOG_DB)
    cursor = connection.cursor()

    cursor.execute(
        """
        CREATE TABLE IF NOT EXISTS device_logs (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            device_name TEXT NOT NULL,
            ip_address TEXT NOT NULL,
            status TEXT NOT NULL,
            latency REAL,
            service_status TEXT NOT NULL,
            timestamp TEXT NOT NULL
        )
        """
    )

    connection.commit()
    connection.close()


def check_port(ip_address, port, timeout=1.0):
    try:
        with socket.create_connection((ip_address, port), timeout=timeout):
            return "open"
    except (socket.timeout, ConnectionRefusedError, OSError):
        return "closed"


def check_service_ports(ip_address, ports=None):
    ports_to_check = ports or SERVICE_PORTS

    return {
        str(port): check_port(ip_address, port)
        for port in ports_to_check
    }


def insert_device_log(device_name, ip_address, status, latency, service_status):
    connection = sqlite3.connect(DEVICE_LOG_DB)
    cursor = connection.cursor()

    cursor.execute(
        """
        INSERT INTO device_logs (
            device_name,
            ip_address,
            status,
            latency,
            service_status,
            timestamp
        )
        VALUES (?, ?, ?, ?, ?, ?)
        """,
        (
            device_name,
            ip_address,
            status,
            latency,
            json.dumps(service_status),
            datetime.now(timezone.utc).isoformat(),
        ),
    )

    connection.commit()
    connection.close()


def build_ping_command(ip_address):
    if platform.system().lower() == "windows":
        return ["ping", "-n", "1", "-w", str(PING_TIMEOUT_SECONDS * 1000), ip_address]

    return ["ping", "-c", "1", "-W", str(PING_TIMEOUT_SECONDS), ip_address]


def parse_latency_ms(output):
    match = re.search(r"time[=<]\s*(\d+(?:\.\d+)?)\s*ms", output, re.IGNORECASE)
    return float(match.group(1)) if match else None


def ping_device(device):
    started_at = time.perf_counter()

    try:
        result = subprocess.run(
            build_ping_command(device.ip_address),
            capture_output=True,
            text=True,
            timeout=PING_TIMEOUT_SECONDS + 1,
        )

        elapsed_ms = round((time.perf_counter() - started_at) * 1000, 2)
        output = f"{result.stdout}\n{result.stderr}"

        if result.returncode == 0:
            return {
                "device_id": device.id,
                "status": "up",
                "latency_ms": parse_latency_ms(output) or elapsed_ms,
                "error": None,
            }

        return {
            "device_id": device.id,
            "status": "down",
            "latency_ms": None,
            "error": output.strip()[:255] or "No ping response",
        }

    except subprocess.TimeoutExpired:
        return {
            "device_id": device.id,
            "status": "down",
            "latency_ms": None,
            "error": "Ping timed out",
        }


def get_consecutive_failures(device_id):
    recent_results = (
        PingResult.query.filter_by(device_id=device_id)
        .order_by(PingResult.checked_at.desc())
        .limit(ALERT_FAILURE_THRESHOLD)
        .all()
    )

    failures = 0

    for result in recent_results:
        if result.status != "down":
            break
        failures += 1

    return failures


def poll_devices():
    """Fetch devices from SQL instead of a hardcoded DEVICES list."""
    with app.app_context():
        devices = Device.query.order_by(Device.id.asc()).all()

        if not devices:
            return

        with ThreadPoolExecutor(max_workers=MAX_WORKERS) as executor:
            ping_results = list(executor.map(ping_device, devices))

        for result in ping_results:
            device = next(
                current_device
                for current_device in devices
                if current_device.id == result["device_id"]
            )
            service_status = check_service_ports(device.ip_address)

            db.session.add(PingResult(**result))
            insert_device_log(
                device_name=device.name,
                ip_address=device.ip_address,
                status=result["status"],
                latency=result["latency_ms"],
                service_status=service_status,
            )

        db.session.commit()


def background_poller():
    while True:
        poll_devices()
        time.sleep(POLL_INTERVAL_SECONDS)


def start_background_poller():
    global poller_started

    if poller_started:
        return

    poller_started = True
    threading.Thread(target=background_poller, daemon=True).start()


@app.route("/")
def index():
    return render_template("index.html")


@app.route("/api/devices")
def api_devices():
    devices = Device.query.order_by(Device.name.asc()).all()
    return jsonify([device.to_dict() for device in devices])


@app.route("/api/main-router/history")
def api_main_router_history():
    main_router = Device.query.filter_by(ip_address="192.168.1.1").first()

    if not main_router:
        return jsonify({"error": "Main router 192.168.1.1 was not found"}), 404

    results = (
        PingResult.query.filter_by(device_id=main_router.id)
        .order_by(PingResult.checked_at.desc())
        .limit(20)
        .all()
    )

    return jsonify([result.to_dict() for result in reversed(results)])


@app.route("/api/devices/<int:device_id>/history")
def api_device_history(device_id):
    results = (
        PingResult.query.filter_by(device_id=device_id)
        .order_by(PingResult.checked_at.desc())
        .limit(60)
        .all()
    )

    return jsonify([result.to_dict() for result in reversed(results)])


@app.route("/api/ping_devices", methods=["POST", "GET"])
def api_ping_devices():
    poll_devices()
    return jsonify({"message": "network sweep completed and logged"})


@app.route("/api/logs", methods=["GET"])
def api_logs():
    limit = request.args.get("limit", 20, type=int)

    if limit < 1:
        limit = 1

    if limit > 1000:
        limit = 1000

    connection = sqlite3.connect(DEVICE_LOG_DB)
    connection.row_factory = sqlite3.Row
    cursor = connection.cursor()

    cursor.execute(
        """
        SELECT
            id,
            device_name,
            ip_address,
            status,
            latency,
            service_status,
            timestamp
        FROM device_logs
        ORDER BY id DESC
        LIMIT ?
        """,
        (limit,),
    )

    rows = cursor.fetchall()
    connection.close()

    logs = []

    for row in rows:
        service_status = json.loads(row["service_status"])

        logs.append(
            {
                "id": row["id"],
                "device_name": row["device_name"],
                "ip_address": row["ip_address"],
                "status": row["status"],
                "latency": row["latency"],
                "service_status": service_status,
                "timestamp": row["timestamp"],
            }
        )

    return jsonify(logs)


with app.app_context():
    db.create_all()
    init_device_logs_db()
    seed_devices()
    start_background_poller()


if __name__ == "__main__":
    app.run(debug=True, use_reloader=False)

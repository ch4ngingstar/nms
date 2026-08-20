"""REST blueprint.

Every route except login is guarded by @require_auth. Serialization is inline
and deliberately small — this surface has no ORM-to-JSON magic to hide a schema
change behind.
"""

from datetime import datetime, timedelta, timezone

from flask import Blueprint, jsonify, request

from protocol.errors import ProtocolError
from server import auth, commands, enrolment
from server.db import db
from server.models import (
    ApObservation, BleObservation, Device, IdsAlert, Job, JobChunk,
    MonitorCycle, MonitorResult, MonitorRollup, Node, Telemetry,
)

api = Blueprint("api", __name__, url_prefix="/api")

RAW_HISTORY_DAYS = 7
MONITOR_INTERVAL_S = 5
DEFAULT_MONITOR_PORTS = [22, 53, 80]


def _iso(value: datetime | None) -> str | None:
    return value.isoformat() if value is not None else None


def _node_summary(node: Node) -> dict:
    return {"node_id": node.node_id, "label": node.label, "state": node.state,
            "capabilities": node.capabilities or [], "fw": node.fw,
            "last_seen": _iso(node.last_seen)}


def _device_dict(device: Device) -> dict:
    return {"id": device.id, "name": device.name, "ip": device.ip,
            "role": device.role, "enabled": device.enabled,
            "node_id": device.node_id}


def _job_dict(job: Job) -> dict:
    return {"job_id": job.job_id, "node_id": job.node_id, "cmd": job.cmd,
            "state": job.state, "gaps": job.gaps, "chunks": job.chunks,
            "results": job.results, "duration_ms": job.duration_ms,
            "error_code": job.error_code, "error_message": job.error_message,
            "created_at": _iso(job.created_at), "finished_at": _iso(job.finished_at)}


# --- auth ------------------------------------------------------------------

@api.post("/login")
def login():
    payload = request.get_json(silent=True) or {}
    if auth.login(payload.get("password", "")):
        return jsonify({"ok": True})
    return jsonify({"error": "invalid credentials"}), 401


@api.post("/logout")
@auth.require_auth
def logout():
    auth.logout()
    return jsonify({"ok": True})


# --- nodes -----------------------------------------------------------------

@api.get("/nodes")
@auth.require_auth
def list_nodes():
    nodes = db.session.execute(db.select(Node)).scalars().all()
    return jsonify([_node_summary(node) for node in nodes])


@api.get("/nodes/<node_id>")
@auth.require_auth
def get_node(node_id):
    node = db.session.get(Node, node_id)
    if node is None:
        return jsonify({"error": "no such node"}), 404
    latest = db.session.execute(
        db.select(Telemetry).filter_by(node_id=node_id)
        .order_by(Telemetry.ts.desc()).limit(1)
    ).scalar_one_or_none()
    body = _node_summary(node)
    body["telemetry"] = None if latest is None else {
        "ts": _iso(latest.ts), "free_heap": latest.free_heap,
        "uptime_s": latest.uptime_s, "rssi": latest.rssi,
        "channel": latest.channel, "jobs_done": latest.jobs_done}
    return jsonify(body)


@api.post("/nodes")
@auth.require_auth
def enrol():
    payload = request.get_json(silent=True) or {}
    try:
        result = enrolment.enrol_node(payload.get("node_id", ""),
                                      payload.get("label"))
    except enrolment.AlreadyEnrolled as exc:
        return jsonify({"error": str(exc)}), 409
    except ProtocolError as exc:
        return jsonify({"error": str(exc)}), 400
    return jsonify(result), 201


# --- devices ---------------------------------------------------------------

@api.get("/devices")
@auth.require_auth
def list_devices():
    devices = db.session.execute(db.select(Device)).scalars().all()
    return jsonify([_device_dict(device) for device in devices])


@api.post("/devices")
@auth.require_auth
def create_device():
    payload = request.get_json(silent=True) or {}
    if not payload.get("name") or not payload.get("ip"):
        return jsonify({"error": "name and ip are required"}), 400
    device = Device(name=payload["name"], ip=payload["ip"],
                    role=payload.get("role", "unknown"),
                    enabled=payload.get("enabled", True),
                    node_id=payload.get("node_id"))
    db.session.add(device)
    db.session.commit()
    _push_monitor_for(device.node_id)
    return jsonify(_device_dict(device)), 201


@api.patch("/devices/<int:device_id>")
@auth.require_auth
def patch_device(device_id):
    device = db.session.get(Device, device_id)
    if device is None:
        return jsonify({"error": "no such device"}), 404
    payload = request.get_json(silent=True) or {}
    for field in ("name", "ip", "role", "enabled", "node_id"):
        if field in payload:
            setattr(device, field, payload[field])
    db.session.commit()
    _push_monitor_for(device.node_id)
    return jsonify(_device_dict(device))


@api.delete("/devices/<int:device_id>")
@auth.require_auth
def delete_device(device_id):
    device = db.session.get(Device, device_id)
    if device is None:
        return jsonify({"error": "no such device"}), 404
    node_id = device.node_id
    db.session.delete(device)
    db.session.commit()
    _push_monitor_for(node_id)
    return "", 204


@api.get("/devices/<int:device_id>/history")
@auth.require_auth
def device_history(device_id):
    if db.session.get(Device, device_id) is None:
        return jsonify({"error": "no such device"}), 404
    cutoff = datetime.now(timezone.utc) - timedelta(days=RAW_HISTORY_DAYS)
    raw = db.session.execute(
        db.select(MonitorCycle.cycle_ts, MonitorResult.status,
                  MonitorResult.latency_ms)
        .join(MonitorResult, MonitorResult.cycle_id == MonitorCycle.id)
        .where(MonitorResult.device_id == device_id)
        .where(MonitorCycle.cycle_ts >= cutoff)
        .order_by(MonitorCycle.cycle_ts)
    ).all()
    rollups = db.session.execute(
        db.select(MonitorRollup).filter_by(device_id=device_id)
        .where(MonitorRollup.hour_ts < cutoff)
        .order_by(MonitorRollup.hour_ts)
    ).scalars().all()
    return jsonify({
        "raw": [{"ts": _iso(ts), "status": status, "latency_ms": latency}
                for ts, status, latency in raw],
        "rollups": [{"hour_ts": _iso(r.hour_ts), "samples": r.samples,
                     "up_count": r.up_count, "latency_min": r.latency_min,
                     "latency_avg": r.latency_avg, "latency_max": r.latency_max}
                    for r in rollups],
    })


# --- jobs ------------------------------------------------------------------

@api.post("/nodes/<node_id>/jobs")
@auth.require_auth
def issue_job(node_id):
    payload = request.get_json(silent=True) or {}
    cmd = payload.get("cmd")
    if not cmd:
        return jsonify({"error": "cmd is required"}), 400
    try:
        job = commands.create_job(node_id, cmd, payload.get("args", {}))
    except commands.UnknownNode as exc:
        return jsonify({"error": str(exc)}), 404
    except (commands.CapabilityError, ProtocolError) as exc:
        return jsonify({"error": str(exc)}), 400
    return jsonify({"job_id": job.job_id, "state": job.state}), 202


@api.get("/jobs/<job_id>")
@auth.require_auth
def get_job(job_id):
    job = db.session.get(Job, job_id)
    if job is None:
        return jsonify({"error": "no such job"}), 404
    return jsonify(_job_dict(job))


@api.get("/jobs/<job_id>/chunks")
@auth.require_auth
def get_job_chunks(job_id):
    if db.session.get(Job, job_id) is None:
        return jsonify({"error": "no such job"}), 404
    chunks = db.session.execute(
        db.select(JobChunk).filter_by(job_id=job_id).order_by(JobChunk.seq)
    ).scalars().all()
    return jsonify([{"seq": c.seq, "payload": c.payload,
                     "received_at": _iso(c.received_at)} for c in chunks])


@api.post("/jobs/<job_id>/cancel")
@auth.require_auth
def cancel_job(job_id):
    job = commands.cancel_job(job_id)
    if job is None:
        return jsonify({"error": "no such job"}), 404
    return jsonify({"job_id": job.job_id, "state": job.state}), 202


# --- rf --------------------------------------------------------------------

@api.get("/rf/aps")
@auth.require_auth
def rf_aps():
    """Access points the fleet sees, with signal strength per probe."""
    observations = db.session.execute(
        db.select(ApObservation).order_by(ApObservation.observed_at.desc())
    ).scalars().all()
    grouped: dict[str, dict] = {}
    for obs in observations:
        entry = grouped.setdefault(obs.bssid, {"bssid": obs.bssid, "ssid": obs.ssid,
                                               "observations": []})
        entry["observations"].append({"node": obs.node_id, "rssi": obs.rssi,
                                       "channel": obs.channel,
                                       "observed_at": _iso(obs.observed_at)})
    return jsonify(list(grouped.values()))


@api.get("/rf/ble")
@auth.require_auth
def rf_ble():
    """BLE devices the fleet sees, with signal strength per probe.

    The ble_scan mirror of /rf/aps: one row per device MAC, its per-node RSSI
    observations nested underneath for the multi-vantage correlation view.
    """
    observations = db.session.execute(
        db.select(BleObservation).order_by(BleObservation.observed_at.desc())
    ).scalars().all()
    grouped: dict[str, dict] = {}
    for obs in observations:
        entry = grouped.setdefault(obs.mac, {
            "mac": obs.mac, "name": obs.name, "manufacturer": obs.manufacturer,
            "connectable": obs.connectable, "observations": []})
        entry["observations"].append({"node": obs.node_id, "rssi": obs.rssi,
                                      "observed_at": _iso(obs.observed_at)})
    return jsonify(list(grouped.values()))


# --- security --------------------------------------------------------------

def _alert_dict(alert: IdsAlert) -> dict:
    return {"id": alert.id, "node_id": alert.node_id, "job_id": alert.job_id,
            "alert_type": alert.alert_type, "source_mac": alert.source_mac,
            "target_mac": alert.target_mac, "channel": alert.channel,
            "count": alert.count, "detected_at": _iso(alert.detected_at)}


@api.get("/security/alerts")
@auth.require_auth
def security_alerts():
    """Wireless IDS alert timeline, newest first.

    Optional ?type= filter narrows to one alert_type; ?node= to one probe.
    """
    query = db.select(IdsAlert).order_by(IdsAlert.detected_at.desc())
    alert_type = request.args.get("type")
    if alert_type:
        query = query.where(IdsAlert.alert_type == alert_type)
    node_id = request.args.get("node")
    if node_id:
        query = query.where(IdsAlert.node_id == node_id)
    alerts = db.session.execute(query).scalars().all()
    return jsonify([_alert_dict(alert) for alert in alerts])


@api.get("/security/rogue-aps")
@auth.require_auth
def security_rogue_aps():
    """The rogue_ap / evil_twin subset of IDS alerts — unexpected APs."""
    alerts = db.session.execute(
        db.select(IdsAlert)
        .where(IdsAlert.alert_type.in_(("rogue_ap", "evil_twin")))
        .order_by(IdsAlert.detected_at.desc())
    ).scalars().all()
    return jsonify([_alert_dict(alert) for alert in alerts])


# --- helpers ---------------------------------------------------------------

def _monitor_args(node_id: str | None) -> dict:
    """Build a set_monitor payload from the devices this node should watch."""
    query = db.select(Device).where(Device.enabled.is_(True))
    if node_id is not None:
        # A device pinned to a node, plus the ones every probe watches (null).
        query = query.where(db.or_(Device.node_id == node_id,
                                   Device.node_id.is_(None)))
    devices = db.session.execute(query).scalars().all()
    return {"enabled": True, "interval_s": MONITOR_INTERVAL_S,
            "devices": [{"id": d.id, "ip": d.ip, "checks": ["ping", "port"],
                         "ports": DEFAULT_MONITOR_PORTS} for d in devices]}


def _push_monitor_for(node_id: str | None) -> None:
    """Push updated monitor config to affected, enrolled nodes.

    A null node_id means every probe watches the device, so the config is
    pushed to the whole fleet; a set node_id targets just that probe.
    """
    if node_id is not None:
        targets = [node_id] if db.session.get(Node, node_id) else []
    else:
        targets = [n.node_id for n in
                   db.session.execute(db.select(Node)).scalars().all()]
    for target in targets:
        try:
            commands.create_job(target, "set_monitor", _monitor_args(target))
        except (commands.CommandError, ProtocolError):
            # A node that cannot take the config (e.g. mid-enrolment) is skipped
            # rather than failing the operator's device edit.
            continue

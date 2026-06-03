"""Handlers for validated protocol messages.

Deliberately free of MQTT and threading: each handler takes an already
validated message dict and writes rows, so protocol semantics are testable
by calling a function. The client that feeds them lives in mqtt_bridge.
"""

from datetime import datetime, timezone

from protocol.job import JobState, JobTracker
from server.db import db
from server.events import bus
from server.models import (
    ApObservation, BleObservation, Device, IdsAlert, Job, JobChunk,
    MonitorCycle, MonitorResult, Node, Telemetry,
)


def to_datetime(epoch_seconds: int) -> datetime:
    """Protocol timestamps are Unix seconds, UTC."""
    return datetime.fromtimestamp(epoch_seconds, tz=timezone.utc)


def get_or_create_node(node_id: str, seen_at: datetime) -> Node:
    """Return the node, creating a minimal row if this is the first sighting.

    The broker ACL already established that this is an authorised node, so
    refusing to record it here would add nothing but lost data.
    """
    node = db.session.get(Node, node_id)
    if node is None:
        node = Node(node_id=node_id, capabilities=[],
                    first_seen=seen_at, last_seen=seen_at)
        db.session.add(node)
    return node


def handle_announce(message: dict) -> Node:
    data = message["data"]
    seen_at = to_datetime(message["ts"])
    node = get_or_create_node(message["node"], seen_at)

    if "label" in data:
        node.label = data["label"]
    node.fw = data["fw"]
    node.chip = data["chip"]
    node.mac = data["mac"]
    node.capabilities = data["capabilities"]
    node.last_seen = seen_at
    db.session.commit()

    bus.publish("node_status", {"node": node.node_id, "state": node.state,
                                "label": node.label,
                                "capabilities": node.capabilities})
    return node


def handle_status(message: dict) -> Node:
    data = message["data"]
    seen_at = to_datetime(message["ts"])
    node = get_or_create_node(message["node"], seen_at)

    node.state = data["state"]
    node.last_status_ts = seen_at
    if data["state"] != "offline":
        # An offline notice is usually the broker's Last Will, published on
        # the node's behalf after it stopped talking. It is not a sign of life.
        node.last_seen = seen_at
    db.session.commit()

    bus.publish("node_status", {"node": node.node_id, "state": node.state,
                                "expect_back_in": data.get("expect_back_in")})
    return node


def handle_telemetry(message: dict) -> Telemetry:
    data = message["data"]
    seen_at = to_datetime(message["ts"])
    node = get_or_create_node(message["node"], seen_at)
    node.last_seen = seen_at

    sample = Telemetry(
        node_id=node.node_id, ts=seen_at,
        free_heap=data["free_heap"], uptime_s=data["uptime_s"],
        rssi=data.get("rssi"), channel=data.get("channel"),
        state=data["state"], jobs_done=data.get("jobs_done"),
    )
    db.session.add(sample)
    db.session.commit()

    bus.publish("telemetry", {"node": node.node_id, "free_heap": data["free_heap"],
                              "uptime_s": data["uptime_s"], "rssi": data.get("rssi")})
    return sample


def handle_monitor(message: dict) -> MonitorCycle:
    data = message["data"]
    cycle_ts = to_datetime(data["cycle_ts"])
    node = get_or_create_node(message["node"], cycle_ts)

    existing = db.session.execute(
        db.select(MonitorCycle).filter_by(node_id=node.node_id, cycle_ts=cycle_ts)
    ).scalar_one_or_none()
    if existing is not None:
        return existing          # QoS 1 redelivery; already recorded

    cycle = MonitorCycle(node_id=node.node_id, cycle_ts=cycle_ts)
    db.session.add(cycle)
    db.session.flush()

    known_device_ids = {
        row[0] for row in db.session.execute(db.select(Device.id)).all()
    }
    stored = 0
    for row in data["results"]:
        # A probe can still be monitoring a device the operator deleted.
        if row["id"] not in known_device_ids:
            continue
        db.session.add(MonitorResult(
            cycle_id=cycle.id, device_id=row["id"], status=row["status"],
            latency_ms=row.get("latency_ms"), ports=row.get("ports"),
        ))
        stored += 1

    node.last_seen = cycle_ts
    db.session.commit()

    bus.publish("monitor_cycle", {"node": node.node_id,
                                  "cycle_ts": data["cycle_ts"],
                                  "results": stored})
    return cycle


def _received_seqs(job_id: str) -> list[int]:
    return [
        row[0] for row in
        db.session.execute(db.select(JobChunk.seq).filter_by(job_id=job_id)).all()
    ]


def _gaps_for(job_id: str) -> list[int]:
    """Reuse the tested tracker rather than reimplementing gap detection."""
    tracker = JobTracker(job_id)
    tracker.accept()
    for seq in _received_seqs(job_id):
        tracker.chunk(seq, {})
    return tracker.gaps


def _project_ap_observations(job: Job, data: dict, observed_at) -> None:
    """Explode a wifi_survey chunk into the queryable observations table.

    Only RF survey gets a projection: GROUP BY bssid comparing rssi across
    node_id is the multi-vantage query that justifies a typed table.
    """
    for access_point in data.get("aps", []):
        db.session.add(ApObservation(
            node_id=job.node_id, job_id=job.job_id,
            bssid=access_point["bssid"], ssid=access_point.get("ssid"),
            channel=access_point.get("channel"), rssi=access_point.get("rssi"),
            auth=access_point.get("auth"), hidden=access_point.get("hidden", False),
            observed_at=observed_at,
        ))


def _project_ble_observations(job: Job, data: dict, observed_at) -> None:
    """Explode a ble_scan chunk into the queryable observations table.

    The ble_scan analogue of _project_ap_observations: multi-vantage RSSI per
    device MAC is the query a single handheld tool cannot answer.
    """
    for device in data.get("devices", []):
        db.session.add(BleObservation(
            node_id=job.node_id, job_id=job.job_id,
            mac=device["mac"], name=device.get("name"),
            rssi=device.get("rssi"), connectable=device.get("connectable", False),
            manufacturer=device.get("manufacturer"),
            observed_at=observed_at,
        ))


def _project_ids_alerts(job: Job, data: dict, detected_at) -> None:
    """Flatten a wifi_ids or ble_scan `alerts` chunk into the typed alert table.

    The alert objects differ by type: a deauth_flood carries source_mac/
    target_mac/count, rogue_ap and evil_twin name the offending AP in `bssid`
    and carry no victim or count, and ble_scan's ble_spam_flood (BLE-IDS) carries
    rate/company_id instead of any MAC. Both AP-identity kinds land their MAC in
    source_mac so the timeline is uniform; unused fields are simply absent from
    the source dict and land as null.
    """
    for alert in data.get("alerts", []):
        db.session.add(IdsAlert(
            node_id=job.node_id, job_id=job.job_id,
            alert_type=alert["type"],
            source_mac=alert.get("source_mac") or alert.get("bssid"),
            target_mac=alert.get("target_mac"),
            channel=alert.get("channel"), count=alert.get("count"),
            rate=alert.get("rate"), company_id=alert.get("company_id"),
            detected_at=detected_at,
        ))


def _store_chunk(job: Job, data: dict, received_at) -> None:
    seq = data["seq"]
    already = db.session.execute(
        db.select(JobChunk.id).filter_by(job_id=job.job_id, seq=seq)
    ).scalar_one_or_none()
    if already is not None:
        return                     # QoS 1 redelivery
    db.session.add(JobChunk(job_id=job.job_id, seq=seq, payload=data,
                            received_at=received_at))
    # Projections key on the chunk's payload shape, not the command name: only
    # wifi_survey emits `aps`, only ble_scan `devices`, only wifi_ids `alerts`
    # (its terminal `frame_stats` chunk carries none and is simply stored raw).
    # Projections added in later phase


def handle_result(message: dict):
    """Drive one job's state machine from a result event."""
    data = message["data"]
    job = db.session.get(Job, data["job_id"])
    if job is None:
        return None                # result for a job this server never issued

    event = data["event"]
    at = to_datetime(message["ts"])
    job.last_event_at = at

    if event == "accepted":
        job.state = JobState.ACCEPTED.value
        job.accepted_at = at
    elif event == "chunk":
        _store_chunk(job, data, at)
    elif event == "done":
        db.session.flush()
        gaps = _gaps_for(job.job_id)
        job.gaps = gaps
        job.state = (JobState.INCOMPLETE if gaps else JobState.DONE).value
        job.chunks = data["chunks"]
        job.results = data["results"]
        job.duration_ms = data["duration_ms"]
        job.finished_at = at
    elif event == "error":
        job.state = JobState.ERROR.value
        job.error_code = data["code"]
        job.error_message = data["message"]
        job.finished_at = at

    db.session.commit()
    bus.publish("job_event", {"job_id": job.job_id, "node": job.node_id,
                              "event": event, "state": job.state})
    return job

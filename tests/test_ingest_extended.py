"""Ingest projection for the Phase-4 radio chunks: ble_scan `devices` and
wifi_ids `alerts`. Mirrors test_ingest_result.py's shape."""

import pytest

from protocol.validate import validate_message
from server.ingest import handle_result, to_datetime
from server.models import (
    BleObservation, CarrierObservation, IdsAlert, Job, JobChunk,
)

TS = 1755302400


def result(data, ts=TS):
    return {"v": 1, "type": "result", "node": "probe-a4c1f8",
            "msg_id": "01J8X2KB0007", "ts": ts, "data": data}


@pytest.fixture
def ble_job(db, node):
    record = Job(job_id="job-ble01", node_id=node.node_id, cmd="ble_scan",
                 args={}, created_at=to_datetime(TS), last_event_at=to_datetime(TS))
    db.session.add(record)
    db.session.commit()
    return record


@pytest.fixture
def rf_job(db, node):
    record = Job(job_id="job-rf001", node_id=node.node_id, cmd="rf_sniff",
                 args={}, created_at=to_datetime(TS), last_event_at=to_datetime(TS))
    db.session.add(record)
    db.session.commit()
    return record


@pytest.fixture
def ids_job(db, node):
    record = Job(job_id="job-ids01", node_id=node.node_id, cmd="wifi_ids",
                 args={}, created_at=to_datetime(TS), last_event_at=to_datetime(TS))
    db.session.add(record)
    db.session.commit()
    return record


# --- ble_scan -> ble_observations ------------------------------------------

def test_devices_chunk_projects_into_ble_observations(db, ble_job):
    handle_result(result({"event": "chunk", "job_id": "job-ble01", "seq": 0,
                          "devices": [
                              {"mac": "4c:11:ae:1a:2b:3c", "name": "Mi Band",
                               "rssi": -72, "connectable": True,
                               "manufacturer": "0157"}]}))
    obs = db.session.query(BleObservation).one()
    assert obs.mac == "4c:11:ae:1a:2b:3c"
    assert obs.name == "Mi Band"
    assert obs.rssi == -72
    assert obs.connectable is True
    assert obs.manufacturer == "0157"
    assert obs.node_id == "probe-a4c1f8"
    assert obs.job_id == "job-ble01"


def test_devices_chunk_also_stored_as_raw_chunk(db, ble_job):
    handle_result(result({"event": "chunk", "job_id": "job-ble01", "seq": 0,
                          "devices": [{"mac": "4c:11:ae:1a:2b:3c", "rssi": -60}]}))
    chunk = db.session.query(JobChunk).one()
    assert chunk.payload["devices"][0]["mac"] == "4c:11:ae:1a:2b:3c"


def test_device_without_name_or_manufacturer_projects_nulls(db, ble_job):
    handle_result(result({"event": "chunk", "job_id": "job-ble01", "seq": 0,
                          "devices": [{"mac": "c0:28:8d:44:55:66", "name": None,
                                       "rssi": -88, "connectable": False}]}))
    obs = db.session.query(BleObservation).one()
    assert obs.name is None
    assert obs.manufacturer is None
    assert obs.connectable is False


def test_empty_devices_chunk_projects_nothing(db, ble_job):
    handle_result(result({"event": "chunk", "job_id": "job-ble01", "seq": 0,
                          "devices": []}))
    assert db.session.query(BleObservation).count() == 0
    assert db.session.query(JobChunk).count() == 1


def test_duplicate_devices_seq_does_not_double_project(db, ble_job):
    payload = {"event": "chunk", "job_id": "job-ble01", "seq": 0,
               "devices": [{"mac": "4c:11:ae:1a:2b:3c", "rssi": -60}]}
    handle_result(result(payload))
    handle_result(result(payload))          # QoS 1 redelivery
    assert db.session.query(BleObservation).count() == 1
    assert db.session.query(JobChunk).count() == 1


# --- rf_sniff -> carrier_observations --------------------------------------

def test_carriers_chunk_projects_into_carrier_observations(db, rf_job):
    handle_result(result({"event": "chunk", "job_id": "job-rf001", "seq": 0,
                          "carriers": [
                              {"freq_mhz": 433.92, "rssi": -64,
                               "bandwidth_khz": 58, "packets": 12}]}))
    obs = db.session.query(CarrierObservation).one()
    assert obs.freq_mhz == 433.92
    assert obs.rssi == -64
    assert obs.bandwidth_khz == 58
    assert obs.packets == 12
    assert obs.node_id == "probe-a4c1f8"
    assert obs.job_id == "job-rf001"


def test_carriers_chunk_also_stored_as_raw_chunk(db, rf_job):
    handle_result(result({"event": "chunk", "job_id": "job-rf001", "seq": 0,
                          "carriers": [{"freq_mhz": 433.92, "rssi": -60}]}))
    chunk = db.session.query(JobChunk).one()
    assert chunk.payload["carriers"][0]["freq_mhz"] == 433.92


def test_carrier_without_optional_fields_projects_nulls(db, rf_job):
    handle_result(result({"event": "chunk", "job_id": "job-rf001", "seq": 0,
                          "carriers": [{"freq_mhz": 868.3, "rssi": -77}]}))
    obs = db.session.query(CarrierObservation).one()
    assert obs.bandwidth_khz is None
    assert obs.packets is None


def test_empty_carriers_chunk_projects_nothing(db, rf_job):
    handle_result(result({"event": "chunk", "job_id": "job-rf001", "seq": 0,
                          "carriers": []}))
    assert db.session.query(CarrierObservation).count() == 0
    assert db.session.query(JobChunk).count() == 1


def test_duplicate_carriers_seq_does_not_double_project(db, rf_job):
    payload = {"event": "chunk", "job_id": "job-rf001", "seq": 0,
               "carriers": [{"freq_mhz": 433.92, "rssi": -60}]}
    handle_result(result(payload))
    handle_result(result(payload))          # QoS 1 redelivery
    assert db.session.query(CarrierObservation).count() == 1
    assert db.session.query(JobChunk).count() == 1


# --- ble_scan alerts (ble_spam_flood) -> ids_alerts -------------------------

def test_ble_spam_alert_chunk_is_schema_valid():
    """A ble_spam_flood alert rides the same generic `alerts` chunk field as
    wifi_ids, so it must validate against the same result schema."""
    validate_message(result({"event": "chunk", "job_id": "job-ble01", "seq": 0,
                             "alerts": [{"type": "ble_spam_flood", "rate": 47,
                                         "company_id": "0x004c"}]}))


def test_ble_spam_alert_projects_into_ids_alerts(db, ble_job):
    handle_result(result({"event": "chunk", "job_id": "job-ble01", "seq": 0,
                          "alerts": [{"type": "ble_spam_flood", "rate": 47,
                                      "company_id": "0x004c"}]}))
    alert = db.session.query(IdsAlert).one()
    assert alert.alert_type == "ble_spam_flood"
    assert alert.rate == 47
    assert alert.company_id == "0x004c"
    assert alert.source_mac is None
    assert alert.target_mac is None
    assert alert.count is None
    assert alert.node_id == "probe-a4c1f8"
    assert alert.job_id == "job-ble01"


def test_ble_spam_alert_also_stored_as_raw_chunk(db, ble_job):
    handle_result(result({"event": "chunk", "job_id": "job-ble01", "seq": 0,
                          "alerts": [{"type": "ble_spam_flood", "rate": 47,
                                      "company_id": "0x004c"}]}))
    chunk = db.session.query(JobChunk).one()
    assert chunk.payload["alerts"][0]["company_id"] == "0x004c"


def test_ble_spam_alert_without_company_id_projects_null(db, ble_job):
    handle_result(result({"event": "chunk", "job_id": "job-ble01", "seq": 0,
                          "alerts": [{"type": "ble_spam_flood", "rate": 55}]}))
    alert = db.session.query(IdsAlert).one()
    assert alert.rate == 55
    assert alert.company_id is None


def test_multiple_ble_spam_alerts_in_one_chunk(db, ble_job):
    handle_result(result({"event": "chunk", "job_id": "job-ble01", "seq": 0,
                          "alerts": [
                              {"type": "ble_spam_flood", "rate": 41,
                               "company_id": "0x004c"},
                              {"type": "ble_spam_flood", "rate": 88,
                               "company_id": "0x00e0"}]}))
    rates = {a.company_id: a.rate for a in db.session.query(IdsAlert).all()}
    assert rates == {"0x004c": 41, "0x00e0": 88}


# --- wifi_ids -> ids_alerts -------------------------------------------------

def test_alerts_chunk_projects_deauth_flood(db, ids_job):
    handle_result(result({"event": "chunk", "job_id": "job-ids01", "seq": 0,
                          "alerts": [
                              {"type": "deauth_flood",
                               "source_mac": "de:ad:be:ef:00:11",
                               "target_mac": "a0:b7:65:11:22:33",
                               "channel": 6, "count": 142,
                               "first_seen": TS - 10, "last_seen": TS}]}))
    alert = db.session.query(IdsAlert).one()
    assert alert.alert_type == "deauth_flood"
    assert alert.source_mac == "de:ad:be:ef:00:11"
    assert alert.target_mac == "a0:b7:65:11:22:33"
    assert alert.channel == 6
    assert alert.count == 142
    assert alert.node_id == "probe-a4c1f8"


def test_rogue_ap_alert_maps_bssid_to_source_mac(db, ids_job):
    """rogue_ap/evil_twin name the AP in `bssid`; it lands in source_mac."""
    handle_result(result({"event": "chunk", "job_id": "job-ids01", "seq": 0,
                          "alerts": [
                              {"type": "rogue_ap", "bssid": "66:77:88:99:aa:bb",
                               "ssid": "Home", "channel": 11, "rssi": -49}]}))
    alert = db.session.query(IdsAlert).one()
    assert alert.alert_type == "rogue_ap"
    assert alert.source_mac == "66:77:88:99:aa:bb"
    assert alert.target_mac is None
    assert alert.count is None


def test_multiple_alerts_in_one_chunk(db, ids_job):
    handle_result(result({"event": "chunk", "job_id": "job-ids01", "seq": 0,
                          "alerts": [
                              {"type": "deauth_flood", "source_mac": "de:ad:be:ef:00:11",
                               "target_mac": "a0:b7:65:11:22:33", "channel": 6,
                               "count": 20},
                              {"type": "evil_twin", "bssid": "66:77:88:99:aa:bb",
                               "ssid": "Home", "expected_bssid": "a0:b7:65:11:22:33",
                               "channel": 11, "rssi": -40}]}))
    types = {a.alert_type for a in db.session.query(IdsAlert).all()}
    assert types == {"deauth_flood", "evil_twin"}


def test_frame_stats_chunk_stores_but_does_not_project(db, ids_job):
    handle_result(result({"event": "chunk", "job_id": "job-ids01", "seq": 1,
                          "frame_stats": {"total": 8213, "management": 1104,
                                          "data": 6890, "control": 219,
                                          "deauth": 142, "probe_request": 57}}))
    assert db.session.query(IdsAlert).count() == 0
    chunk = db.session.query(JobChunk).one()
    assert chunk.payload["frame_stats"]["deauth"] == 142


def test_alerts_projection_is_isolated_from_ble(db, ids_job, ble_job):
    handle_result(result({"event": "chunk", "job_id": "job-ids01", "seq": 0,
                          "alerts": [{"type": "rogue_ap", "bssid": "66:77:88:99:aa:bb",
                                      "channel": 1}]}))
    handle_result(result({"event": "chunk", "job_id": "job-ble01", "seq": 0,
                          "devices": [{"mac": "4c:11:ae:1a:2b:3c", "rssi": -60}]}))
    assert db.session.query(IdsAlert).count() == 1
    assert db.session.query(BleObservation).count() == 1

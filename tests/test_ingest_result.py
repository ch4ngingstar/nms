import pytest

from server.ingest import handle_result, to_datetime
from server.models import ApObservation, Job, JobChunk

TS = 1755302400


def result(data, ts=TS):
    return {"v": 1, "type": "result", "node": "probe-a4c1f8",
            "msg_id": "01J8X2KB0001", "ts": ts, "data": data}


@pytest.fixture
def job(db, node):
    # Pin the timestamps to the message epoch so ordering assertions do not
    # depend on the wall clock, which sits a year ahead of TS in this env.
    record = Job(job_id="job-7f3a91", node_id=node.node_id,
                 cmd="port_scan", args={},
                 created_at=to_datetime(TS), last_event_at=to_datetime(TS))
    db.session.add(record)
    db.session.commit()
    return record


def test_accepted_sets_state(db, job):
    handle_result(result({"event": "accepted", "job_id": "job-7f3a91"}))
    assert db.session.get(Job, "job-7f3a91").state == "accepted"
    assert db.session.get(Job, "job-7f3a91").accepted_at is not None


def test_chunk_is_stored(db, job):
    handle_result(result({"event": "chunk", "job_id": "job-7f3a91", "seq": 0,
                          "open": [{"host": "10.0.0.5", "port": 22}]}))
    chunk = db.session.query(JobChunk).one()
    assert chunk.seq == 0
    assert chunk.payload["open"][0]["port"] == 22


def test_duplicate_seq_is_ignored(db, job):
    payload = {"event": "chunk", "job_id": "job-7f3a91", "seq": 0, "open": []}
    handle_result(result(payload))
    handle_result(result(payload))
    assert db.session.query(JobChunk).count() == 1


def test_chunk_advances_last_event_at(db, job):
    before = job.last_event_at
    handle_result(result({"event": "chunk", "job_id": "job-7f3a91", "seq": 0,
                          "open": []}, ts=TS + 60))
    assert db.session.get(Job, "job-7f3a91").last_event_at > before


def test_contiguous_chunks_finish_done(db, job):
    for seq in range(3):
        handle_result(result({"event": "chunk", "job_id": "job-7f3a91",
                              "seq": seq, "open": []}))
    handle_result(result({"event": "done", "job_id": "job-7f3a91",
                          "chunks": 3, "results": 3, "duration_ms": 812}))
    stored = db.session.get(Job, "job-7f3a91")
    assert stored.state == "done"
    assert stored.gaps == []
    assert stored.duration_ms == 812
    assert stored.finished_at is not None


def test_sequence_gap_finishes_incomplete(db, job):
    """Spec §7.4: a truncated scan must announce itself, not look complete."""
    for seq in (0, 2):
        handle_result(result({"event": "chunk", "job_id": "job-7f3a91",
                              "seq": seq, "open": []}))
    handle_result(result({"event": "done", "job_id": "job-7f3a91",
                          "chunks": 2, "results": 2, "duration_ms": 500}))
    stored = db.session.get(Job, "job-7f3a91")
    assert stored.state == "incomplete"
    assert stored.gaps == [1]


def test_error_is_terminal(db, job):
    handle_result(result({"event": "error", "job_id": "job-7f3a91",
                          "code": "busy", "message": "a job is already running"}))
    stored = db.session.get(Job, "job-7f3a91")
    assert stored.state == "error"
    assert stored.error_code == "busy"
    assert stored.finished_at is not None


def test_result_for_unknown_job_is_dropped(db, node):
    assert handle_result(result({"event": "accepted", "job_id": "job-nope"})) is None


def test_wifi_survey_chunks_project_into_observations(db, node):
    """The multi-vantage correlation dataset (spec §6.2)."""
    db.session.add(Job(job_id="job-rf", node_id=node.node_id,
                       cmd="wifi_survey", args={}))
    db.session.commit()
    handle_result({"v": 1, "type": "result", "node": "probe-a4c1f8",
                   "msg_id": "01J8X2KB0006", "ts": TS,
                   "data": {"event": "chunk", "job_id": "job-rf", "seq": 0,
                            "aps": [{"bssid": "a0:b7:65:11:22:33", "ssid": "Home",
                                     "channel": 6, "rssi": -61, "auth": "wpa2",
                                     "hidden": False}]}})
    observation = db.session.query(ApObservation).one()
    assert observation.bssid == "a0:b7:65:11:22:33"
    assert observation.rssi == -61
    assert observation.node_id == "probe-a4c1f8"


def test_port_scan_chunks_do_not_project(db, job):
    handle_result(result({"event": "chunk", "job_id": "job-7f3a91", "seq": 0,
                          "open": [{"host": "10.0.0.5", "port": 22}]}))
    assert db.session.query(ApObservation).count() == 0

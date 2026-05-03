import pytest

from protocol.errors import ProtocolError
from protocol.job import JobState, JobTracker


def test_new_job_is_pending():
    assert JobTracker("job-1").state is JobState.PENDING


def test_accept_moves_to_accepted():
    job = JobTracker("job-1")
    job.accept()
    assert job.state is JobState.ACCEPTED


def test_contiguous_chunks_then_done():
    job = JobTracker("job-1")
    job.accept()
    for seq in range(3):
        job.chunk(seq, {"open": [{"port": 22}]})
    job.finish(chunks=3, results=3, duration_ms=100)
    assert job.state is JobState.DONE
    assert job.gaps == []


def test_sequence_gap_marks_incomplete():
    job = JobTracker("job-1")
    job.accept()
    job.chunk(0, {})
    job.chunk(2, {})
    job.finish(chunks=2, results=2, duration_ms=100)
    assert job.state is JobState.INCOMPLETE
    assert job.gaps == [1]


def test_multiple_gaps_recorded():
    job = JobTracker("job-1")
    job.accept()
    job.chunk(0, {})
    job.chunk(4, {})
    job.finish(chunks=2, results=2, duration_ms=100)
    assert job.gaps == [1, 2, 3]


def test_duplicate_seq_ignored_not_counted_twice():
    job = JobTracker("job-1")
    job.accept()
    job.chunk(0, {})
    job.chunk(0, {})
    assert job.received == 1


def test_error_is_terminal():
    job = JobTracker("job-1")
    job.accept()
    job.fail("busy", "a job is already running")
    assert job.state is JobState.ERROR
    assert job.error_code == "busy"


def test_chunk_after_terminal_rejected():
    job = JobTracker("job-1")
    job.accept()
    job.finish(chunks=0, results=0, duration_ms=10)
    with pytest.raises(ProtocolError):
        job.chunk(0, {})


def test_timeout_is_terminal():
    job = JobTracker("job-1")
    job.accept()
    job.time_out()
    assert job.state is JobState.TIMED_OUT


def test_negative_seq_rejected():
    job = JobTracker("job-1")
    job.accept()
    with pytest.raises(ProtocolError):
        job.chunk(-1, {})

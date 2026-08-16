"""Server-side tracking of a single job's result stream (spec §7.4)."""

from enum import Enum

from protocol.errors import ProtocolError


class JobState(Enum):
    PENDING = "pending"
    ACCEPTED = "accepted"
    DONE = "done"
    INCOMPLETE = "incomplete"
    ERROR = "error"
    TIMED_OUT = "timed_out"


_TERMINAL = {JobState.DONE, JobState.INCOMPLETE, JobState.ERROR, JobState.TIMED_OUT}


class JobTracker:
    """Accumulates result events for one job and detects sequence gaps."""

    def __init__(self, job_id: str):
        self.job_id = job_id
        self.state = JobState.PENDING
        self.error_code: str | None = None
        self.summary: dict | None = None
        self._seen: set[int] = set()

    @property
    def received(self) -> int:
        return len(self._seen)

    @property
    def gaps(self) -> list[int]:
        """Sequence numbers missing below the highest one received."""
        if not self._seen:
            return []
        return [n for n in range(max(self._seen)) if n not in self._seen]

    def _guard_open(self) -> None:
        if self.state in _TERMINAL:
            raise ProtocolError(
                f"job {self.job_id} is terminal ({self.state.value}); no further events"
            )

    def accept(self) -> None:
        self._guard_open()
        self.state = JobState.ACCEPTED

    def chunk(self, seq: int, payload: dict) -> None:
        self._guard_open()
        if not isinstance(seq, int) or seq < 0:
            raise ProtocolError(f"invalid seq {seq!r}: expected a non-negative integer")
        self._seen.add(seq)

    def finish(self, chunks: int, results: int, duration_ms: int) -> None:
        self._guard_open()
        self.summary = {"chunks": chunks, "results": results, "duration_ms": duration_ms}
        self.state = JobState.INCOMPLETE if self.gaps else JobState.DONE

    def fail(self, code: str, message: str) -> None:
        self._guard_open()
        self.error_code = code
        self.summary = {"code": code, "message": message}
        self.state = JobState.ERROR

    def time_out(self) -> None:
        self._guard_open()
        self.state = JobState.TIMED_OUT

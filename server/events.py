"""In-memory pub/sub feeding SSE subscribers.

The bus is process-local, which is why the server must run as a single
process.
"""

import queue
import threading

MAX_QUEUE = 500


class EventBus:
    """Fan-out to SSE clients that never blocks on a slow consumer."""

    def __init__(self, maxsize: int = MAX_QUEUE):
        self._maxsize = maxsize
        self._subscribers: set[queue.Queue] = set()
        self._lock = threading.Lock()

    def subscribe(self) -> queue.Queue:
        subscriber = queue.Queue(maxsize=self._maxsize)
        with self._lock:
            self._subscribers.add(subscriber)
        return subscriber

    def unsubscribe(self, subscriber: queue.Queue) -> None:
        with self._lock:
            self._subscribers.discard(subscriber)

    @property
    def subscriber_count(self) -> int:
        with self._lock:
            return len(self._subscribers)

    def publish(self, event_type: str, payload: dict) -> None:
        """Deliver to every subscriber, dropping any whose queue is full."""
        message = {"type": event_type, "data": payload}
        with self._lock:
            targets = list(self._subscribers)
        for subscriber in targets:
            try:
                subscriber.put_nowait(message)
            except queue.Full:
                self.unsubscribe(subscriber)


bus = EventBus()

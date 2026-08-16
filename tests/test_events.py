import queue

import pytest

from server.events import MAX_QUEUE, EventBus


def test_subscriber_receives_published_event():
    bus = EventBus()
    q = bus.subscribe()
    bus.publish("node_status", {"node": "probe-a4c1f8"})
    assert q.get_nowait() == {"type": "node_status",
                              "data": {"node": "probe-a4c1f8"}}


def test_every_subscriber_receives_the_event():
    bus = EventBus()
    first, second = bus.subscribe(), bus.subscribe()
    bus.publish("telemetry", {"free_heap": 1})
    assert first.get_nowait()["type"] == "telemetry"
    assert second.get_nowait()["type"] == "telemetry"


def test_unsubscribe_stops_delivery():
    bus = EventBus()
    q = bus.subscribe()
    bus.unsubscribe(q)
    bus.publish("telemetry", {})
    with pytest.raises(queue.Empty):
        q.get_nowait()


def test_publish_with_no_subscribers_is_harmless():
    EventBus().publish("telemetry", {})


def test_slow_subscriber_is_dropped_when_queue_fills():
    """A browser on a suspended laptop must not grow the server's memory."""
    bus = EventBus(maxsize=3)
    q = bus.subscribe()
    for _ in range(3):
        bus.publish("telemetry", {})
    assert bus.subscriber_count == 1
    bus.publish("telemetry", {})          # overflows
    assert bus.subscriber_count == 0


def test_default_queue_bound():
    assert MAX_QUEUE == 500

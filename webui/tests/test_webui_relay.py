"""Tests for webui's UDP -> WebSocket relay logic.

No FastAPI/uvicorn server or real SDR needed here -- these exercise
Relay's fan-out and station-list-refresh behavior directly, with a real
loopback UDP socket as the "SDR" and fake WebSocket objects as
subscribers, matching this project's existing preference for measuring
real behavior over assuming it.
"""

import asyncio
import socket

import httpx
import pytest

# webui.relay imports webui.aac_encoder at module scope, which imports PyAV
# (a `webui` extra). Skip the whole module cleanly when it isn't installed,
# same as test_webui_aac.py -- these tests don't exercise the AAC path.
pytest.importorskip("av")

import webui.relay as relay_module  # noqa: E402
from webui.relay import Relay, StationInfo, _StationProtocol  # noqa: E402


class FakeWebSocket:
    def __init__(self):
        self.received: list[bytes | str] = []
        self.closed_with: tuple | None = None

    async def send_bytes(self, data: bytes) -> None:
        self.received.append(data)

    async def send_text(self, data: str) -> None:
        self.received.append(data)

    async def close(self, code: int | None = None, reason: str | None = None) -> None:
        self.closed_with = (code, reason)


def test_fan_out_to_multiple_subscribers():
    async def run():
        # rx_url's host must match the real UDP sends' source address
        # below (loopback) -- the source check drops anything else.
        relay = Relay(rx_url="http://127.0.0.1:1")
        port = 54501
        await relay._apply_active_stations(
            {port: StationInfo(port=port, label="Test", freq=100_000_000, stereo=False)}
        )
        ws1, ws2 = FakeWebSocket(), FakeWebSocket()
        await relay.subscribe(port, ws1)
        await relay.subscribe(port, ws2)

        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        payloads = [f"chunk-{i}".encode() for i in range(5)]
        for p in payloads:
            sock.sendto(p, ("127.0.0.1", port))
        await asyncio.sleep(0.3)  # let the per-subscriber drain tasks run

        assert ws1.received == payloads
        assert ws2.received == payloads
        await relay.stop()

    asyncio.run(run())


def test_unsubscribe_stops_delivery():
    async def run():
        relay = Relay(rx_url="http://127.0.0.1:1")
        port = 54502
        await relay._apply_active_stations(
            {port: StationInfo(port=port, label="Test", freq=100_000_000, stereo=False)}
        )
        ws = FakeWebSocket()
        await relay.subscribe(port, ws)
        relay.unsubscribe(port, ws)

        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.sendto(b"should-not-arrive", ("127.0.0.1", port))
        await asyncio.sleep(0.3)

        assert ws.received == []
        await relay.stop()

    asyncio.run(run())


def test_station_removal_closes_transport_and_subscribers():
    async def run():
        relay = Relay(rx_url="http://rx.test")
        port = 54503
        await relay._apply_active_stations(
            {port: StationInfo(port=port, label="Test", freq=100_000_000, stereo=False)}
        )
        ws = FakeWebSocket()
        await relay.subscribe(port, ws)
        assert port in relay._transports

        await relay._apply_active_stations({})

        assert port not in relay._transports
        assert port not in relay._subscribers
        assert ws.closed_with == (1001, "station no longer active")

    asyncio.run(run())


def test_station_format_change_disconnects_subscribers_but_keeps_transport():
    async def run():
        relay = Relay(rx_url="http://rx.test")
        port = 54505
        mono_info = StationInfo(port=port, label="Test", freq=100_000_000, stereo=False)
        stereo_info = StationInfo(
            port=port, label="Test", freq=100_000_000, stereo=True
        )

        await relay._apply_active_stations({port: mono_info})
        ws = FakeWebSocket()
        await relay.subscribe(port, ws)
        original_transport = relay._transports[port]

        # Station stays on the same port, but flips mono -> stereo (e.g.
        # stations.yml edited while a client is already connected).
        await relay._apply_active_stations({port: stereo_info})

        assert ws.closed_with == (1001, "station format changed, reconnect")
        # the UDP listener itself is untouched -- only the metadata
        # changed, not which port the station streams on.
        assert relay._transports[port] is original_transport
        assert relay.active_stations() == {port: stereo_info}

        await relay.stop()

    asyncio.run(run())


class SlowWebSocket(FakeWebSocket):
    """send_bytes() blocks until .gate is set -- stands in for a
    stalled/slow PCM subscriber."""

    def __init__(self):
        super().__init__()
        self.gate = asyncio.Event()

    async def send_bytes(self, data: bytes) -> None:
        await self.gate.wait()
        await super().send_bytes(data)


def test_slow_subscriber_does_not_block_others():
    """A stalled subscriber's own backlog must not delay delivery to
    another subscriber on the same station -- each has its own queue +
    drain task now, rather than one task per datagram touching every
    subscriber in sequence."""

    async def run():
        relay = Relay(rx_url="http://127.0.0.1:1")
        port = 54506
        await relay._apply_active_stations(
            {port: StationInfo(port=port, label="Test", freq=100_000_000, stereo=False)}
        )
        slow, fast = SlowWebSocket(), FakeWebSocket()
        await relay.subscribe(port, slow)
        await relay.subscribe(port, fast)

        protocol = _StationProtocol(port, relay._subscribers, relay, relay._pcm_queues)
        protocol.datagram_received(b"payload", ("127.0.0.1", 40000))
        await asyncio.sleep(0.05)  # let both drain tasks get scheduled

        assert fast.received == [b"payload"]
        assert slow.received == []  # still parked in send_bytes(), not lost

        slow.gate.set()
        await asyncio.sleep(0.05)
        assert slow.received == [b"payload"]
        await relay.stop()

    asyncio.run(run())


def test_datagram_received_rejects_wrong_source():
    """These sockets bind 0.0.0.0, so anything
    not from rx itself must be dropped rather than fanned out to every
    listener."""

    async def run():
        relay = Relay(rx_url="http://127.0.0.1:1")  # _rx_host == "127.0.0.1"
        port = 54507
        await relay._apply_active_stations(
            {port: StationInfo(port=port, label="Test", freq=100_000_000, stereo=False)}
        )
        ws = FakeWebSocket()
        await relay.subscribe(port, ws)

        protocol = _StationProtocol(port, relay._subscribers, relay, relay._pcm_queues)
        protocol.datagram_received(b"forged", ("10.0.0.99", 9999))
        await asyncio.sleep(0.05)

        assert ws.received == []
        await relay.stop()

    asyncio.run(run())


def test_pcm_queue_drops_oldest_on_overflow():
    """Same drop-oldest shape as the AAC egress/ingest queues -- a
    subscriber whose queue fills up loses its oldest buffered datagrams,
    not its newest."""

    async def run():
        relay = Relay(rx_url="http://127.0.0.1:1")
        port = 54508
        await relay._apply_active_stations(
            {port: StationInfo(port=port, label="Test", freq=100_000_000, stereo=False)}
        )
        ws = FakeWebSocket()
        relay._subscribers.setdefault(port, set()).add(ws)
        # A tiny queue with no drain task consuming it, so the overflow
        # math is deterministic instead of racing a concurrent drain.
        q: asyncio.Queue = asyncio.Queue(maxsize=3)
        relay._pcm_queues[ws] = q

        protocol = _StationProtocol(port, relay._subscribers, relay, relay._pcm_queues)
        for i in range(5):
            protocol.datagram_received(f"chunk-{i}".encode(), ("127.0.0.1", 1))

        remaining = [q.get_nowait() for _ in range(q.qsize())]
        assert remaining == [b"chunk-2", b"chunk-3", b"chunk-4"]
        await relay.stop()

    asyncio.run(run())


class FakeResponse:
    def __init__(self, payload):
        self._payload = payload

    def raise_for_status(self) -> None:
        pass

    def json(self):
        return self._payload


class FakeHTTP:
    """Stands in for relay._http (an httpx.AsyncClient) in refresh_once."""

    def __init__(self, payload):
        self._payload = payload
        self.requested_url: str | None = None

    async def get(self, url: str, **kwargs):
        self.requested_url = url
        return FakeResponse(self._payload)

    async def aclose(self) -> None:
        pass


def test_refresh_once_pulls_station_list_from_rx_daemon():
    async def run():
        relay = Relay(rx_url="http://rx.test/")
        relay._http = FakeHTTP(
            {
                "audio_rate": 48_000,
                "status_port": 5599,
                "stations": [
                    {
                        "port": 54507,
                        "label": "A",
                        "freq": 100_000_000,
                        "stereo": False,
                        "stereo_mode": "off",
                    }
                ],
            }
        )

        await relay.refresh_once()

        assert relay._http.requested_url == "http://rx.test/api/stations/active"
        assert relay.audio_rate == 48_000
        assert relay.status_port == 5599
        assert relay.active_stations() == {
            54507: StationInfo(
                port=54507, label="A", freq=100_000_000, stereo=False, stereo_mode="off"
            )
        }

        await relay.stop()

    asyncio.run(run())


def test_refresh_once_keeps_previous_list_when_rx_has_no_plan():
    async def run():
        relay = Relay(rx_url="http://rx.test")
        port = 54508
        info = StationInfo(port=port, label="A", freq=100_000_000, stereo=False)
        await relay._apply_active_stations({port: info})

        # the rx daemon returns this shape when it can't resolve a valid plan.
        relay._http = FakeHTTP({"audio_rate": None, "status_port": 0, "stations": []})
        await relay.refresh_once()

        assert relay.active_stations() == {port: info}
        await relay.stop()

    asyncio.run(run())


def test_refresh_once_empty_station_list_tears_everything_down():
    """rx can now run with zero stations (stations.yml empty / scan-only).
    The plan is still valid -- audio_rate is set -- so this is *not* the
    'no plan' case; the relay must apply the empty set and drop all ports,
    not hold the previous list."""

    async def run():
        relay = Relay(rx_url="http://rx.test")
        port = 54508
        info = StationInfo(port=port, label="A", freq=100_000_000, stereo=False)
        await relay._apply_active_stations({port: info})

        relay._http = FakeHTTP(
            {"audio_rate": 32_000, "status_port": 8082, "stations": []}
        )
        await relay.refresh_once()

        assert relay.audio_rate == 32_000
        assert relay.active_stations() == {}
        await relay.stop()

    asyncio.run(run())


def test_station_added_after_removed_gets_a_fresh_transport():
    async def run():
        relay = Relay(rx_url="http://rx.test")
        port = 54504
        info = StationInfo(port=port, label="Test", freq=100_000_000, stereo=False)

        await relay._apply_active_stations({port: info})
        assert port in relay._transports

        await relay._apply_active_stations({})
        assert port not in relay._transports

        await relay._apply_active_stations({port: info})
        assert port in relay._transports
        assert relay.active_stations() == {port: info}

        await relay.stop()

    asyncio.run(run())


def test_stats_broadcast_to_subscribers():
    async def run():
        relay = Relay(rx_url="http://rx.test")
        ws1, ws2 = FakeWebSocket(), FakeWebSocket()
        await relay.subscribe_stats(ws1)
        await relay.subscribe_stats(ws2)

        relay._stats = {"sdr": {"link_up": True}, "stations": []}
        await relay._broadcast_stats()

        expected = ['{"enabled": true, "sdr": {"link_up": true}, "stations": []}']
        assert ws1.received == expected
        assert ws2.received == expected
        await relay.stop()

    asyncio.run(run())


def test_stats_dead_subscriber_pruned():
    async def run():
        relay = Relay(rx_url="http://rx.test")

        class DeadWebSocket(FakeWebSocket):
            async def send_text(self, data: str) -> None:
                raise ConnectionError("gone")

        dead = DeadWebSocket()
        alive = FakeWebSocket()
        await relay.subscribe_stats(dead)
        await relay.subscribe_stats(alive)

        relay._stats = {"sdr": None, "stations": []}
        await relay._broadcast_stats()

        assert dead not in relay._stats_subs
        assert alive in relay._stats_subs
        assert alive.received  # the surviving subscriber still got the payload
        await relay.stop()

    asyncio.run(run())


def test_codec_flip_pushes_reload_event():
    """A codec flip must tell open listener pages
    to reload rather than leave them retrying a 409 against the endpoint
    they can no longer use."""

    async def run():
        relay = Relay(rx_url="http://rx.test")
        stats_ws = FakeWebSocket()
        await relay.subscribe_stats(stats_ws)

        await relay._apply_active_stations({}, codec_flip=True)

        assert stats_ws.received == ['{"type": "codec_changed"}']
        await relay.stop()

    asyncio.run(run())


def test_station_set_change_pushes_reload_event():
    """An admin add/remove must reach pages that
    were already open, not just the relay's own next poll."""

    async def run():
        relay = Relay(rx_url="http://rx.test")
        stats_ws = FakeWebSocket()
        port = 54503
        await relay._apply_active_stations(
            {port: StationInfo(port=port, label="A", freq=100_000_000, stereo=False)}
        )
        await relay.subscribe_stats(stats_ws)  # after the initial set, like a real page

        await relay._apply_active_stations(
            {
                port: StationInfo(port=port, label="A", freq=100_000_000, stereo=False),
                port
                + 1: StationInfo(
                    port=port + 1, label="B", freq=101_000_000, stereo=False
                ),
            }
        )

        assert stats_ws.received == ['{"type": "stations_changed"}']
        await relay.stop()

    asyncio.run(run())


def test_unchanged_station_set_does_not_push_reload_event():
    """A refresh that reports the same port set (e.g. a metadata-only
    change, or just a poll with nothing new) must not reload every open
    tab for no reason."""

    async def run():
        relay = Relay(rx_url="http://rx.test")
        stats_ws = FakeWebSocket()
        port = 54504
        await relay._apply_active_stations(
            {port: StationInfo(port=port, label="A", freq=100_000_000, stereo=False)}
        )
        await relay.subscribe_stats(stats_ws)

        await relay._apply_active_stations(
            {port: StationInfo(port=port, label="A", freq=100_000_000, stereo=False)}
        )

        assert stats_ws.received == []
        await relay.stop()

    asyncio.run(run())


def test_status_ws_sends_snapshot_on_connect():
    """A client connecting to /ws/status gets the latest stats immediately,
    without waiting for the next _stats_loop tick."""
    from fastapi.testclient import TestClient

    from webui.app import create_app

    app = create_app("http://rx.test")
    relay = Relay("http://rx.test")
    relay.status_port = 8600
    relay._stats = {"sdr": {"link_up": True}, "stations": []}
    app.state.relay = relay
    client = TestClient(app)  # no `with`: don't run the lifespan / relay.start

    with client.websocket_connect("/ws/status") as ws:
        assert ws.receive_json() == {
            "enabled": True,
            "sdr": {"link_up": True},
            "stations": [],
        }
    assert relay._stats_subs == set()  # cleaned up on disconnect


def test_status_ws_disabled_state():
    """No stats yet (status port disabled, or the daemon hasn't answered
    the first poll) yields the same disabled shape as GET /api/status."""
    from fastapi.testclient import TestClient

    from webui.app import create_app

    app = create_app("http://rx.test")
    relay = Relay("http://rx.test")
    app.state.relay = relay
    client = TestClient(app)  # no `with`: don't run the lifespan / relay.start

    with client.websocket_connect("/ws/status") as ws:
        assert ws.receive_json() == {"enabled": False, "sdr": None, "stations": []}


def test_station_audio_ws_rejects_before_audio_rate_is_known():
    """A connection landing before rx's plan first resolves (audio_rate
    still None) must be rejected, not sent stream_info with a null
    sample_rate that would throw client-side building an AudioContext."""
    from fastapi.testclient import TestClient
    from starlette.websockets import WebSocketDisconnect

    from webui.app import create_app

    app = create_app("http://rx.test")
    relay = Relay("http://rx.test")
    relay.audio_rate = None
    relay._active = {
        55611: StationInfo(port=55611, label="A", freq=100_000_000, stereo=False)
    }
    app.state.relay = relay
    client = TestClient(app)  # no `with`: don't run the lifespan / relay.start

    with pytest.raises(WebSocketDisconnect):
        with client.websocket_connect("/ws/stations/55611/audio"):
            pass
    assert 55611 not in relay._subscribers


# --- listener-gated decode: POST /active + decode-linger -----------------


class FakePostResponse:
    def __init__(self, status_code: int = 200):
        self.status_code = status_code

    def raise_for_status(self) -> None:
        if self.status_code >= 400:
            raise RuntimeError(f"HTTP {self.status_code}")


class FakePoster:
    """Records POST bodies sent to rx's /active; .get is inert."""

    def __init__(self, status_code: int = 200):
        self.status_code = status_code
        self.posts: list[dict] = []

    async def get(self, url: str, **kwargs):
        return FakeResponse({})

    async def post(self, url: str, json=None, **kwargs):
        self.posts.append({"url": url, "json": json, "auth": kwargs.get("auth")})
        return FakePostResponse(self.status_code)

    async def aclose(self) -> None:
        pass


async def _stations(relay, *ports):
    await relay._apply_active_stations(
        {
            p: StationInfo(port=p, label=f"S{p}", freq=100_000_000, stereo=False)
            for p in ports
        }
    )


def test_push_active_set_posts_ports_with_live_listeners():
    async def run():
        relay = Relay(rx_url="http://rx.test")
        relay._http = FakePoster()
        relay.status_port = 9001
        await _stations(relay, 41001, 41002)
        await relay.subscribe(41001, FakeWebSocket())

        await relay._decode_gate._push()

        assert relay._http.posts[-1]["url"] == "http://rx.test:9001/active"
        assert relay._http.posts[-1]["json"] == {"ports": [41001]}
        await relay.stop()

    asyncio.run(run())


def test_decode_linger_keeps_port_briefly_after_last_listener_leaves(monkeypatch):
    async def run():
        # Linger only briefly so the test can observe expiry for real,
        # same pattern as test_aac_encoder_lazy_lifecycle.
        monkeypatch.setattr("webui.relay.DECODE_LINGER_SEC", 0.05)
        relay = Relay(rx_url="http://rx.test")
        relay._http = FakePoster()
        relay.status_port = 9001
        await _stations(relay, 42001)

        ws = FakeWebSocket()
        await relay.subscribe(42001, ws)
        assert relay._decode_gate._decode_ports() == [42001]

        relay.unsubscribe(42001, ws)
        # still decoded immediately after the last listener goes
        assert relay._decode_gate._decode_ports() == [42001]

        await asyncio.sleep(0.15)  # let the linger timer fire
        assert relay._decode_gate._decode_ports() == []
        await relay.stop()

    asyncio.run(run())


def test_push_active_set_sends_no_auth_without_admin_password():
    async def run():
        relay = Relay(rx_url="http://rx.test")
        relay._http = FakePoster()
        relay.status_port = 9001
        await _stations(relay, 45001)
        await relay.subscribe(45001, FakeWebSocket())

        assert relay_module.RX_ADMIN_PASSWORD == ""
        await relay._decode_gate._push()

        assert relay._http.posts[-1]["auth"] is httpx.USE_CLIENT_DEFAULT
        await relay.stop()

    asyncio.run(run())


def test_push_active_set_sends_basic_auth_with_admin_password():
    async def run():
        relay = Relay(rx_url="http://rx.test")
        relay._http = FakePoster()
        relay.status_port = 9001
        await _stations(relay, 46001)
        await relay.subscribe(46001, FakeWebSocket())

        relay_module.RX_ADMIN_PASSWORD = "hunter2"
        try:
            await relay._decode_gate._push()
        finally:
            relay_module.RX_ADMIN_PASSWORD = ""

        assert relay._http.posts[-1]["auth"] == ("", "hunter2")
        await relay.stop()

    asyncio.run(run())


def test_push_active_set_quiet_when_receiver_not_gating():
    async def run():
        relay = Relay(rx_url="http://rx.test")
        relay._http = FakePoster()
        relay.status_port = 9001
        relay._stats = {"decode": {"listener_gated": False}}
        await _stations(relay, 43001)
        await relay.subscribe(43001, FakeWebSocket())

        await relay._decode_gate._push()

        assert relay._http.posts == []
        await relay.stop()

    asyncio.run(run())


def test_push_active_set_survives_404_from_receiver():
    async def run():
        relay = Relay(rx_url="http://rx.test")
        relay._http = FakePoster(status_code=404)
        relay.status_port = 9001
        await _stations(relay, 44001)
        await relay.subscribe(44001, FakeWebSocket())

        await relay._decode_gate._push()  # must not raise

        assert len(relay._http.posts) == 1
        await relay.stop()

    asyncio.run(run())

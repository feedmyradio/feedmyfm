"""Tests for webui's AAC-LC listener path (config ``listener.codec: aac``).

Two layers, both measuring real behaviour like the rest of the suite:

  * :class:`~webui.aac_encoder.StationEncoder` on its own -- ADTS framing,
    no-loss accumulation across ragged datagram boundaries;
  * the relay wiring -- lazy per-listener encoder lifecycle, HTTP fan-out,
    prime buffer, pcm/aac coexistence, format/codec-flip teardown,
    bounded egress queues.

Needs PyAV (a `webui` extra); skipped if it isn't importable.
"""

import array
import asyncio
import io
import math
import socket

import pytest

av = pytest.importorskip("av")

from webui.aac_encoder import AAC_FRAME_SAMPLES, StationEncoder  # noqa: E402
from webui.relay import AAC_SUB_QUEUE_MAXSIZE, Relay, StationInfo  # noqa: E402

RATE = 32_000
AAC_LISTENER = {"codec": "aac", "aac_bitrate_mono": 96000, "aac_bitrate_stereo": 128000}


def sine_s16(nsamp: int, channels: int, *, freq=440.0, amp=8000, phase=0) -> bytes:
    a = array.array("h")
    for i in range(nsamp):
        v = int(math.sin(2 * math.pi * freq * (i + phase) / RATE) * amp)
        v = max(-32768, min(32767, v))
        for _ in range(channels):
            a.append(v)
    return a.tobytes()


def parse_adts(frame: bytes) -> dict:
    assert frame[0] == 0xFF and (frame[1] & 0xF0) == 0xF0, "no ADTS syncword"
    profile = (frame[2] >> 6) & 0x3
    sfi = (frame[2] >> 2) & 0xF
    chan_cfg = ((frame[2] & 0x1) << 2) | ((frame[3] >> 6) & 0x3)
    frame_len = ((frame[3] & 0x3) << 11) | (frame[4] << 3) | ((frame[5] >> 5) & 0x7)
    return {
        "profile": profile,
        "sfi": sfi,
        "channels": chan_cfg,
        "frame_len": frame_len,
    }


def decode_adts(stream: bytes) -> tuple[int, int, int]:
    """(total samples, rate, channels) from a concatenated ADTS stream."""
    container = av.open(io.BytesIO(stream), format="aac")
    st = container.streams.audio[0]
    total = 0
    for packet in container.demux(st):
        for fr in packet.decode():
            total += fr.samples
    return total, st.rate, st.channels


# --------------------------------------------------------------------------
# StationEncoder
# --------------------------------------------------------------------------


@pytest.mark.parametrize("stereo", [False, True])
def test_adts_framing(stereo):
    enc = StationEncoder(stereo=stereo, bitrate=128000 if stereo else 96000)
    channels = 2 if stereo else 1
    frames = enc.feed(sine_s16(RATE, channels))  # 1 s
    frames += enc.close()
    assert len(frames) >= 25

    for f in frames:
        h = parse_adts(f)
        assert h["profile"] == 1  # AAC-LC (object type 2 -> profile field 1)
        assert h["sfi"] == 5  # 32 kHz
        assert h["channels"] == channels
        assert h["frame_len"] == len(f)


@pytest.mark.parametrize("stereo", [False, True])
def test_frame_boundary_accumulation(stereo):
    """Datagram-sized feed() calls whose sample counts aren't multiples of
    1024 must lose no samples across the batch: the same total audio fed
    as many ragged chunks or as one block yields the same frame count."""
    channels = 2 if stereo else 1
    total_samples = 40 * 329  # 329 samples/datagram is what a 1316 B MTU gives stereo

    ragged = StationEncoder(stereo=stereo, bitrate=96000)
    n_ragged = 0
    for d in range(40):
        n_ragged += len(ragged.feed(sine_s16(329, channels, phase=d * 329)))

    one_shot = StationEncoder(stereo=stereo, bitrate=96000)
    n_one = len(one_shot.feed(sine_s16(total_samples, channels)))

    assert n_ragged == n_one
    # and it's the expected count for that many samples (encoder holds back
    # ~2 frames of lookahead, so allow a small slack).
    assert 0 <= (total_samples // AAC_FRAME_SAMPLES) - n_ragged <= 3


def test_encoder_output_decodes_back_to_the_right_length():
    enc = StationEncoder(stereo=True, bitrate=128000)
    fed = 2 * RATE  # 2 s stereo
    stream = b"".join(enc.feed(sine_s16(fed, 2)) + enc.close())
    total, rate, channels = decode_adts(stream)
    assert rate == RATE
    assert channels == 2
    # AAC-LC adds encoder priming/padding; a few frames of slack either way.
    assert abs(total - fed) <= 4 * AAC_FRAME_SAMPLES


# --------------------------------------------------------------------------
# Relay wiring
# --------------------------------------------------------------------------


def _relay(codec_aac: bool = True) -> Relay:
    # Host must match the real UDP sends' source address (loopback) used
    # throughout this file -- the source check in _StationProtocol
    # otherwise drops every datagram as not-from-rx.
    r = Relay(rx_url="http://127.0.0.1:1")
    r.audio_rate = RATE
    r.listener = dict(AAC_LISTENER) if codec_aac else {"codec": "pcm"}
    return r


async def _drain(q: asyncio.Queue) -> list:
    out = []
    while not q.empty():
        item = q.get_nowait()
        if item is None:
            break
        out.append(item)
    return out


def test_aac_encoder_lazy_lifecycle(monkeypatch):
    async def run():
        # Linger the encoder only briefly so the test can observe teardown.
        monkeypatch.setattr("webui.relay.AAC_ENCODER_LINGER_SEC", 0.05)
        relay = _relay()
        port = 55601
        info = StationInfo(port=port, label="A", freq=100_000_000, stereo=False)
        await relay._apply_active_stations({port: info})

        assert port not in relay._aac_pool._encoders  # no listener yet

        q = await relay.subscribe_aac(port)
        assert q is not None
        assert port in relay._aac_pool._encoders
        assert port in relay._aac_pool._enc_tasks

        relay.unsubscribe_aac(port, q)
        # Encoder is kept warm (lingering) for a moment, not dropped at once.
        assert port in relay._aac_pool._encoders
        assert port in relay._aac_pool._linger

        # A reconnect within the linger window reuses the same encoder and
        # cancels the teardown.
        q2 = await relay.subscribe_aac(port)
        assert port not in relay._aac_pool._linger
        relay.unsubscribe_aac(port, q2)

        await asyncio.sleep(0.15)  # let the linger timer fire
        assert port not in relay._aac_pool._encoders
        assert port not in relay._aac_pool._enc_tasks
        assert port not in relay._aac_pool._linger

        await relay.stop()

    asyncio.run(run())


def test_aac_http_fan_out():
    async def run():
        relay = _relay()
        port = 55602
        info = StationInfo(port=port, label="A", freq=100_000_000, stereo=False)
        await relay._apply_active_stations({port: info})

        q1 = await relay.subscribe_aac(port)
        q2 = await relay.subscribe_aac(port)

        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        # ~1.5 s of mono audio in 658-sample (1316 B) datagrams
        for d in range(72):
            sock.sendto(sine_s16(658, 1, phase=d * 658), ("127.0.0.1", port))
        await asyncio.sleep(0.6)

        f1 = await _drain(q1)
        f2 = await _drain(q2)
        assert len(f1) >= 20
        assert f1 == f2  # identical ordered frame sequence to every listener

        total, rate, channels = decode_adts(b"".join(f1))
        assert rate == RATE and channels == 1
        assert total >= 40 * AAC_FRAME_SAMPLES  # roughly the audio we fed

        await relay.stop()

    asyncio.run(run())


def test_aac_prime_buffer(monkeypatch):
    # Pin the prime depth -- it's env-tunable (FEEDMYFM_AAC_PRIME_FRAMES)
    # and may be 0 in a given deployment.
    monkeypatch.setattr("webui.relay.AAC_PRIME_FRAMES", 24)

    async def run():
        relay = _relay()
        port = 55603
        info = StationInfo(port=port, label="A", freq=100_000_000, stereo=False)
        await relay._apply_active_stations({port: info})

        q_early = await relay.subscribe_aac(port)

        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        for d in range(72):
            sock.sendto(sine_s16(658, 1, phase=d * 658), ("127.0.0.1", port))
        await asyncio.sleep(0.6)
        await _drain(q_early)  # early listener has consumed everything so far

        # A listener joining mid-stream is handed the recent backlog
        # immediately, before any new datagram arrives.
        q_late = await relay.subscribe_aac(port)
        primed = q_late.qsize()
        assert 0 < primed <= 24  # bounded by the (monkeypatched) prime depth
        for f in await _drain(q_late):
            parse_adts(f)  # all well-formed ADTS

        await relay.stop()

    asyncio.run(run())


def test_pcm_and_aac_coexist():
    async def run():
        relay = _relay()
        port = 55604
        info = StationInfo(port=port, label="A", freq=100_000_000, stereo=False)
        await relay._apply_active_stations({port: info})

        class FakeWS:
            def __init__(self):
                self.received = []

            async def send_bytes(self, data):
                self.received.append(data)

        ws = FakeWS()
        await relay.subscribe(port, ws)
        q = await relay.subscribe_aac(port)

        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        payloads = [sine_s16(658, 1, phase=d * 658) for d in range(48)]
        for p in payloads:
            sock.sendto(p, ("127.0.0.1", port))
        await asyncio.sleep(0.5)

        assert ws.received == payloads  # PCM path byte-for-byte unchanged
        assert len(await _drain(q)) >= 15  # AAC path also producing

        await relay.stop()

    asyncio.run(run())


def test_format_change_recreates_encoder():
    async def run():
        relay = _relay()
        port = 55605
        mono = StationInfo(port=port, label="A", freq=100_000_000, stereo=False)
        stereo = StationInfo(port=port, label="A", freq=100_000_000, stereo=True)

        await relay._apply_active_stations({port: mono})
        q_mono = await relay.subscribe_aac(port)
        assert relay._aac_pool._encoders[port].channels == 1

        # mono -> stereo on the same port tears the encoder down and ends
        # the open response (the generator sees the None sentinel).
        await relay._apply_active_stations({port: stereo})
        assert port not in relay._aac_pool._encoders
        assert q_mono.get_nowait() is None

        q_stereo = await relay.subscribe_aac(port)
        assert relay._aac_pool._encoders[port].channels == 2

        relay.unsubscribe_aac(port, q_stereo)
        await relay.stop()

    asyncio.run(run())


def test_codec_flip_disconnects_all_subscribers():
    async def run():
        relay = _relay(codec_aac=False)
        port = 55606
        info = StationInfo(port=port, label="A", freq=100_000_000, stereo=False)
        await relay._apply_active_stations({port: info})

        class FakeWS:
            def __init__(self):
                self.closed_with = None

            async def send_bytes(self, data):
                pass

            async def close(self, code=None, reason=None):
                self.closed_with = (code, reason)

        ws = FakeWS()
        await relay.subscribe(port, ws)

        # the rx daemon now reports listener.codec: aac -> every open pcm
        # socket must be dropped so the page reconnects to audio.aac.
        relay._http = _FakeHTTP(
            {
                "audio_rate": RATE,
                "status_port": 0,
                "listener": dict(AAC_LISTENER),
                "stations": [
                    {
                        "port": port,
                        "label": "A",
                        "freq": 100_000_000,
                        "stereo": False,
                        "stereo_mode": "off",
                    }
                ],
            }
        )
        await relay.refresh_once()

        assert ws.closed_with == (1001, "listener codec changed, reconnect")
        assert relay.listener["codec"] == "aac"

        await relay.stop()

    asyncio.run(run())


def test_bounded_egress_queue_drops_oldest():
    async def run():
        relay = _relay()
        port = 55607
        info = StationInfo(port=port, label="A", freq=100_000_000, stereo=False)
        await relay._apply_active_stations({port: info})

        q = await relay.subscribe_aac(port)

        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        # Flood well past the queue bound without ever draining q.
        for d in range(400):
            sock.sendto(sine_s16(658, 1, phase=d * 658), ("127.0.0.1", port))
        await asyncio.sleep(0.8)

        assert q.qsize() <= AAC_SUB_QUEUE_MAXSIZE  # memory stays bounded

        await relay.stop()

    asyncio.run(run())


def test_audio_aac_route_guards():
    """The audio.aac endpoint 404s for an unknown port and 409s when the
    system is in pcm mode -- both return before any streaming starts. (The
    happy path is an endless stream, exercised at the relay level above.)"""
    from fastapi.testclient import TestClient

    from webui.app import create_app

    app = create_app("http://rx.test")
    relay = Relay("http://rx.test")
    relay.audio_rate = RATE
    app.state.relay = relay
    client = TestClient(app)  # no `with`: don't run the lifespan / relay.start

    relay.listener = {"codec": "pcm"}
    relay._active = {}
    assert client.get("/stations/55609/audio.aac").status_code == 404

    relay._active = {
        55609: StationInfo(port=55609, label="A", freq=100_000_000, stereo=False)
    }
    assert client.get("/stations/55609/audio.aac").status_code == 409
    assert 55609 not in relay._aac_pool._encoders  # 409 path never built an encoder


def test_audio_aac_503s_before_audio_rate_is_known():
    """A connection landing before rx's plan first resolves (audio_rate is
    still None) must not build an encoder guessing AAC_RATE for the real
    station rate -- 503 instead."""
    from fastapi.testclient import TestClient

    from webui.app import create_app

    app = create_app("http://rx.test")
    relay = Relay("http://rx.test")
    relay.audio_rate = None
    relay.listener = {"codec": "aac"}
    relay._active = {
        55610: StationInfo(port=55610, label="A", freq=100_000_000, stereo=False)
    }
    app.state.relay = relay
    client = TestClient(app)  # no `with`: don't run the lifespan / relay.start

    resp = client.get("/stations/55610/audio.aac")
    assert resp.status_code == 503
    assert 55610 not in relay._aac_pool._encoders


@pytest.mark.parametrize("codec", ["pcm", "aac"])
def test_index_template_carries_listener_mode(codec):
    """index.html stamps the mode on <body> and emits per-station artwork
    + the hidden <audio> element the aac path reuses."""
    from fastapi.testclient import TestClient

    from webui.app import create_app

    app = create_app("http://rx.test")
    relay = Relay("http://rx.test")
    relay.audio_rate = RATE
    relay.listener = {
        "codec": codec,
        "aac_bitrate_mono": 96000,
        "aac_bitrate_stereo": 128000,
    }
    relay._active = {
        7355: StationInfo(port=7355, label="Test FM", freq=94_200_000, stereo=False)
    }
    app.state.relay = relay
    client = TestClient(app)  # no `with`: skip the lifespan / relay.start
    from webui.routes.listener import _SITE_COOKIE, _SITE_TOKEN

    client.cookies.set(_SITE_COOKIE, _SITE_TOKEN)  # past the splash gate

    html = client.get("/").text
    assert f'data-listener-codec="{codec}"' in html
    assert '<audio id="aac-audio"' in html
    assert 'data-artwork="/static/logos/_default.png"' in html


class _FakeHTTP:
    def __init__(self, payload):
        self._payload = payload

    async def get(self, url, **kwargs):
        class _R:
            def __init__(self, p):
                self._p = p

            def raise_for_status(self):
                pass

            def json(self):
                return self._p

        return _R(self._payload)

    async def aclose(self):
        pass

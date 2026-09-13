"""
Per-station UDP -> browser audio relay.

feedmyfm's own network.udp_sink blocks send raw signed-16-bit-LE PCM
(mono, or interleaved L/R for stereo stations) to config.udp.host, one
UDP datagram per MTU-sized chunk, no header. This module listens on
each currently-active station's port and, depending on the system-wide
``listener.codec``:

  * ``pcm`` -- fans each incoming datagram out unmodified to every
    browser websocket subscribed to that station (no transcoding, lowest
    added latency);
  * ``aac`` -- additionally, for any station with a live AAC listener,
    runs the datagrams through a lazily-created :class:`StationEncoder`
    and fans the resulting ADTS frames to the station's open
    ``audio.aac`` HTTP responses. The PCM websocket path stays wired in
    both modes (the ``?pcm=1`` debug escape hatch rides on it).

Station list + audio_rate + the ``listener`` block come from the
feedmyfm-rx daemon's ``GET /api/stations/active`` (unauthenticated:
non-sensitive metadata), polled on a timer (REFRESH_INTERVAL_SEC). The
daemon resolves that plan in-process -- the single source of frequency
planning and validation -- so this service holds no config files and
never shells out; it just reads back the resolved plan over HTTP.

``Relay``'s responsibilities are split across three collaborators:

  * :class:`LingerSet` -- one shared "keep this key alive a bit longer"
    helper, used by both the rx decode-linger and the AAC
    encoder-linger.
  * :class:`AacEncoderPool` -- owns every AAC encoder + its queues.
  * :class:`DecodeGateClient` -- owns the ``POST /active`` push loop.

``Relay`` itself keeps the UDP transports, PCM fan-out, and the
station-list/stats refresh loops, and orchestrates the two collaborators
above.
"""

import asyncio
import collections
import concurrent.futures
import json
import logging
import os
import socket
from dataclasses import dataclass
from typing import Callable, Hashable
from urllib.parse import urlsplit

import httpx
from starlette.websockets import WebSocket

from webui.aac_encoder import AAC_RATE, StationEncoder

logger = logging.getLogger(__name__)

REFRESH_INTERVAL_SEC = 12.0
STATS_INTERVAL_SEC = 3.0

# Listener-gated decode (rx config `decode.listener_gated`): we push the
# set of station ports that currently have a live listener to the rx
# daemon's `POST /active`, and it skips the per-station DSP chain for the
# rest. No effect unless the receiver has the toggle on -- POST /active is
# a 404 otherwise, which we treat as "feature off" and go quiet.
#
# Re-push the set at least this often regardless of subscribe/unsubscribe
# churn, so a dropped POST self-heals well inside rx's 30 s fail-open
# window and an expired decode-linger is dropped promptly.
ACTIVE_PUSH_KEEPALIVE_SEC = 5.0

# Keep a station in the pushed active set this long after its last
# listener leaves, so a page refresh / media-element reconnect resumes
# into an already-warm decode chain (pilot PLL locked, de-emphasis / AGC /
# squelch settled) instead of a cold start. The rx-side mirror of
# AAC_ENCODER_LINGER_SEC. Override with FEEDMYFM_DECODE_LINGER_SEC; 0
# disables the linger.
DECODE_LINGER_SEC = max(0.0, float(os.environ.get("FEEDMYFM_DECODE_LINGER_SEC", "30")))

# rx's admin password (same var, same value, as the receiver's own
# FEEDMYFM_RX_ADMIN_PASSWORD): POST /active needs it whenever rx's
# status.host is non-loopback (the split-host topology). Loopback rx
# doesn't check it, so sending it there too is harmless. Empty => omit
# the Authorization header; rx then 401s if it actually required one.
RX_ADMIN_PASSWORD = os.environ.get("FEEDMYFM_RX_ADMIN_PASSWORD", "")

# `pcm` fail-safe default, mirrored from rx admin_routes.hpp.
LISTENER_DEFAULT = {
    "codec": "pcm",
    "aac_bitrate_mono": 96000,
    "aac_bitrate_stereo": 128000,
}

# Per-encoder ingest queue (raw PCM datagrams awaiting encode) and
# per-HTTP-response egress queue (ADTS frames awaiting the browser).
# Both bounded + drop-oldest so one stuck consumer can't grow memory.
AAC_ENC_QUEUE_MAXSIZE = 64
AAC_SUB_QUEUE_MAXSIZE = 256

# Per-PCM-websocket egress queue -- same
# bounded + drop-oldest shape as the AAC egress queue above, so one
# stalled PCM subscriber's own backlog can't grow without bound or
# block delivery to anyone else on the same station. ~100+ datagrams/s
# per station (stereo roughly double mono); 256 is a few seconds of
# slack before a genuinely stuck client starts losing audio.
PCM_SUB_QUEUE_MAXSIZE = 256

# ADTS frame = 1024 samples = 32 ms at 32 kHz. On connect the encoder's
# last N frames are handed to the <audio> element as one instant burst.
#
# This helps Android/desktop media elements start promptly. iOS Safari,
# though, wall-clock pre-buffers ~5 s of any bare chunked stream before it
# will play -- it ignores the burst (measured: buffered stays empty for
# ~5 s regardless of how much we send, then jumps). So on iOS the burst
# does nothing for startup and only adds standing latency (you begin at
# the back of an N-second buffer). Keep it modest.
#
# Tune without a rebuild via FEEDMYFM_AAC_PRIME_FRAMES on the webui
# container. 24 ~= 0.75 s; 0 = pure live.
AAC_PRIME_FRAMES = max(0, int(os.environ.get("FEEDMYFM_AAC_PRIME_FRAMES", "24")))

# Keep a station's encoder (warm, and with its prime buffer full) this
# long after the last AAC listener disconnects. A page refresh or the
# media element's own reconnect then resumes instantly off the primed
# encoder instead of cold-starting -- which iOS often gives up on.
AAC_ENCODER_LINGER_SEC = 45.0

# Sentinel pushed into an egress queue to tell the audio.aac generator to
# end (station gone, format flip, or codec flip).
_AAC_EOS = None


@dataclass(frozen=True)
class StationInfo:
    port: int
    label: str
    freq: int
    stereo: bool  # emits interleaved L/R frames (stereo_mode is "on" or "auto")
    stereo_mode: str = "off"  # "off" | "on" | "auto"


class LingerSet:
    """Keeps a set of keys "alive" for a given number of seconds past the
    last time they were armed, expiring via a real timer rather than a
    lazily-checked deadline. Shared by both the rx decode-linger and the
    AAC encoder-linger.

    ``arm(key, seconds, on_expire=None)`` (re)starts key's timer, calling
    ``on_expire`` (if given) exactly once when it fires; the AAC pool uses
    that to actually tear the lingering encoder down. Decode-linger has
    nothing to tear down -- it only cares whether a key is still ``in``
    this set -- so it arms with no callback at all.
    """

    def __init__(self) -> None:
        self._timers: dict[Hashable, asyncio.TimerHandle] = {}

    def arm(
        self, key: Hashable, seconds: float, on_expire: Callable[[], None] | None = None
    ) -> None:
        self.cancel(key)
        if seconds <= 0:
            return

        def _expire() -> None:
            self._timers.pop(key, None)
            if on_expire is not None:
                on_expire()

        self._timers[key] = asyncio.get_running_loop().call_later(seconds, _expire)

    def cancel(self, key: Hashable) -> None:
        """Stop key's linger immediately (e.g. a new subscriber arrived)
        -- does *not* call on_expire."""
        timer = self._timers.pop(key, None)
        if timer is not None:
            timer.cancel()

    def cancel_all(self) -> None:
        for key in list(self._timers):
            self.cancel(key)

    def active(self) -> set:
        """Keys currently armed and not yet expired."""
        return set(self._timers)

    def __contains__(self, key: Hashable) -> bool:
        return key in self._timers


class _StationProtocol(asyncio.DatagramProtocol):
    """One instance per active station port; fans datagrams out to subscribers.

    Each subscriber has its own bounded, drop-oldest egress queue
    (`pcm_queues`, same shape as the AAC path's) drained by a dedicated
    task (`Relay._pcm_drain`). `datagram_received`
    itself only ever does non-blocking `put_nowait`s, so a stalled
    subscriber's own backlog can never block delivery to anyone else on the
    same station or block the transport from receiving the next datagram;
    no per-datagram task needed either.
    """

    def __init__(
        self,
        port: int,
        subscribers: dict[int, set[WebSocket]],
        relay: "Relay | None" = None,
        pcm_queues: "dict[WebSocket, asyncio.Queue] | None" = None,
    ):
        self._port = port
        self._subscribers = subscribers
        # Optional so the existing relay tests can still build a protocol
        # with just (port, subscribers); the AAC ingest hop and the source
        # check below are skipped when there's no relay to reach the
        # encoder queues / expected rx address through.
        self._relay = relay
        self._pcm_queues = pcm_queues if pcm_queues is not None else {}

    def datagram_received(self, data: bytes, addr) -> None:
        # These sockets bind 0.0.0.0, so on a
        # multi-homed or LAN-reachable webui host anyone who can reach this
        # port could otherwise inject audio fanned out to every listener.
        # rx is the only legitimate sender.
        if self._relay is not None and addr[0] != self._relay._rx_host:
            return
        for ws in list(self._subscribers.get(self._port, ())):
            q = self._pcm_queues.get(ws)
            if q is None:
                continue
            try:
                q.put_nowait(data)
            except asyncio.QueueFull:
                try:
                    q.get_nowait()
                    q.put_nowait(data)
                except (asyncio.QueueEmpty, asyncio.QueueFull):
                    pass
        # AAC ingest: same-thread, non-blocking hand-off of the raw
        # datagram to this station's encoder task, if one exists.
        if self._relay is not None:
            self._relay._feed_encoder(self._port, data)


class AacEncoderPool:
    """Owns every AAC encoder + its ingest/egress queues + prime buffer,
    keyed by station port. An encoder exists
    only while >=1 ``audio.aac`` response is open for that port;
    AAC_ENCODER_LINGER_SEC keeps a just-emptied one warm for a bit so a
    quick reconnect (a page refresh, the media element's own retry)
    resumes instantly instead of cold-starting.

    ``get_listener``/``get_audio_rate``: the caller's *current* values at
    encoder-creation time -- they can change over the Relay's lifetime
    (a fresh `/api/stations/active` poll), so this pool reads them via
    callback rather than owning a stale copy.
    """

    def __init__(
        self,
        get_listener: Callable[[], dict],
        get_audio_rate: Callable[[], int | None],
    ):
        self._get_listener = get_listener
        self._get_audio_rate = get_audio_rate
        self._aac_subs: dict[int, set[asyncio.Queue]] = {}
        self._encoders: dict[int, StationEncoder] = {}
        self._enc_queues: dict[int, asyncio.Queue] = {}
        self._enc_tasks: dict[int, asyncio.Task] = {}
        self._prime: dict[int, collections.deque] = {}
        self._linger = LingerSet()
        # Shared across encoders; PyAV releases the GIL inside encode().
        self._encode_pool = concurrent.futures.ThreadPoolExecutor(
            thread_name_prefix="aac-encode"
        )

    def has_subscribers(self, port: int) -> bool:
        return bool(self._aac_subs.get(port))

    def active_ports(self) -> set[int]:
        return {p for p, s in self._aac_subs.items() if s}

    async def subscribe(self, station: StationInfo) -> asyncio.Queue:
        """Register an ``audio.aac`` HTTP response for ``station.port``.
        Returns its egress queue, already seeded with the prime backlog.
        Builds the encoder on the first subscriber."""
        port = station.port
        self._linger.cancel(port)
        first = not self._aac_subs.get(port)
        q: asyncio.Queue = asyncio.Queue(maxsize=AAC_SUB_QUEUE_MAXSIZE)
        self._aac_subs.setdefault(port, set()).add(q)
        if first:
            self._make_encoder(station)
        for frame in list(self._prime.get(port, ())):
            q.put_nowait(frame)
        return q

    def unsubscribe(self, port: int, q: asyncio.Queue) -> None:
        subs = self._aac_subs.get(port)
        if subs is None:
            return
        subs.discard(q)
        if not subs:
            self._aac_subs.pop(port, None)
            # Don't tear the encoder down straight away -- keep it warm
            # and priming for a bit so a refresh / media-element
            # reconnect resumes instantly (see AAC_ENCODER_LINGER_SEC).
            if port in self._encoders and port not in self._linger:
                self._linger.arm(
                    port, AAC_ENCODER_LINGER_SEC, lambda: self.teardown(port)
                )

    def drop(self, port: int) -> None:
        """End every open audio.aac response for a port and tear the
        encoder down (station gone / format flip / codec flip)."""
        for q in self._aac_subs.pop(port, set()):
            try:
                q.put_nowait(_AAC_EOS)
            except asyncio.QueueFull:
                pass
        self.teardown(port)

    def _make_encoder(self, station: StationInfo) -> None:
        if station.port in self._encoders:
            return
        listener = self._get_listener()
        bitrate = (
            listener.get("aac_bitrate_stereo", 128000)
            if station.stereo
            else listener.get("aac_bitrate_mono", 96000)
        )
        self._encoders[station.port] = StationEncoder(
            stereo=station.stereo,
            bitrate=bitrate,
            in_rate=self._get_audio_rate() or AAC_RATE,
        )
        self._enc_queues[station.port] = asyncio.Queue(maxsize=AAC_ENC_QUEUE_MAXSIZE)
        self._prime[station.port] = collections.deque(maxlen=AAC_PRIME_FRAMES)
        self._enc_tasks[station.port] = asyncio.create_task(self._drain(station.port))
        logger.info(
            "AAC encoder started for port %s (%s, %d bit/s)",
            station.port,
            "stereo" if station.stereo else "mono",
            bitrate,
        )

    def teardown(self, port: int) -> None:
        self._linger.cancel(port)
        task = self._enc_tasks.pop(port, None)
        if task is not None:
            task.cancel()
        self._enc_queues.pop(port, None)
        self._prime.pop(port, None)
        enc = self._encoders.pop(port, None)
        if enc is not None:
            enc.close()
            logger.info("AAC encoder stopped for port %s", port)

    def feed(self, port: int, data: bytes) -> None:
        """Non-blocking hand-off of a raw PCM datagram to a station's
        encoder task. Drop-oldest if the encoder has fallen behind."""
        q = self._enc_queues.get(port)
        if q is None:
            return
        try:
            q.put_nowait(data)
        except asyncio.QueueFull:
            try:
                q.get_nowait()
                q.put_nowait(data)
            except (asyncio.QueueEmpty, asyncio.QueueFull):
                pass

    async def _drain(self, port: int) -> None:
        """One task per encoder: pull PCM datagrams, encode off the event
        loop, fan the ADTS frames to the port's audio.aac responses and
        the prime buffer."""
        q = self._enc_queues[port]
        enc = self._encoders[port]
        loop = asyncio.get_running_loop()
        while True:
            data = await q.get()
            try:
                frames = await loop.run_in_executor(self._encode_pool, enc.feed, data)
            except Exception:
                logger.exception("AAC encode failed on port %s", port)
                continue
            if not frames:
                continue
            prime = self._prime.get(port)
            if prime is not None:
                prime.extend(frames)
            for sub_q in list(self._aac_subs.get(port, ())):
                for frame in frames:
                    try:
                        sub_q.put_nowait(frame)
                    except asyncio.QueueFull:
                        try:
                            sub_q.get_nowait()
                            sub_q.put_nowait(frame)
                        except (asyncio.QueueEmpty, asyncio.QueueFull):
                            pass

    def shutdown(self) -> None:
        for port in list(self._encoders):
            self.teardown(port)
        self._linger.cancel_all()
        self._encode_pool.shutdown(wait=False)


class DecodeGateClient:
    """Pushes the set of stations with a live listener to rx's
    ``POST /active`` (listener-gated decode) -- rx skips the per-station
    DSP chain for anything not in that set. No effect unless rx's
    ``decode.listener_gated`` is on; a 404 means it's off, and this goes
    quiet until told otherwise.

    Reads the Relay's current state through the injected getters rather
    than owning any of it -- station list, live PCM+AAC listener ports,
    and the latest status-port poll all change out from under this class
    on the Relay's own schedule.
    """

    def __init__(
        self,
        get_http: Callable[[], "httpx.AsyncClient | None"],
        rx_host: str,
        get_status_port: Callable[[], int],
        get_active_ports: Callable[[], set[int]],
        get_listener_ports: Callable[[], set[int]],
        get_stats: Callable[[], dict],
    ):
        self._get_http = get_http
        self._rx_host = rx_host
        self._get_status_port = get_status_port
        self._get_active_ports = get_active_ports
        self._get_listener_ports = get_listener_ports
        self._get_stats = get_stats
        self._linger = LingerSet()
        self._dirty = asyncio.Event()
        self._task: asyncio.Task | None = None

    def start(self) -> None:
        self._task = asyncio.create_task(self._loop())
        self._dirty.set()  # push an initial (empty) set right away

    async def stop(self) -> None:
        if self._task is not None:
            self._task.cancel()
            await asyncio.gather(self._task, return_exceptions=True)
        self._linger.cancel_all()

    def start_linger(self, port: int) -> None:
        """A port's last listener just left -- keep decoding it for a bit
        (DECODE_LINGER_SEC) so a quick reconnect skips the cold start."""
        if DECODE_LINGER_SEC > 0:
            self._linger.arm(port, DECODE_LINGER_SEC)

    def cancel_linger(self, port: int) -> None:
        self._linger.cancel(port)

    def kick(self) -> None:
        """Ask the push loop to push the set now; coalesces churn."""
        self._dirty.set()

    def _decode_ports(self) -> list[int]:
        """The set to push to rx: live-listener ports plus ports still
        inside their decode-linger window, restricted to active stations."""
        ports = self._get_listener_ports() | self._linger.active()
        return sorted(ports & self._get_active_ports())

    async def _loop(self) -> None:
        while True:
            try:
                await asyncio.wait_for(
                    self._dirty.wait(), timeout=ACTIVE_PUSH_KEEPALIVE_SEC
                )
            except asyncio.TimeoutError:
                pass  # keepalive tick
            self._dirty.clear()
            await self._push()

    async def _push(self) -> None:
        http = self._get_http()
        status_port = self._get_status_port()
        if not status_port or http is None:
            return
        # Stay quiet once we know the receiver isn't gating -- POST /active
        # 404s then. An explicit False in the status blob is trusted; an
        # absent/unknown value still gets a push (bootstrapping).
        decode = (self._get_stats() or {}).get("decode")
        if isinstance(decode, dict) and decode.get("listener_gated") is False:
            return
        try:
            r = await http.post(
                f"http://{self._rx_host}:{status_port}/active",
                json={"ports": self._decode_ports()},
                auth=(
                    ("", RX_ADMIN_PASSWORD)
                    if RX_ADMIN_PASSWORD
                    else httpx.USE_CLIENT_DEFAULT
                ),
                timeout=2.0,
            )
            if r.status_code == 404:
                return  # decode.listener_gated off on the receiver
            r.raise_for_status()
        except Exception as e:
            logger.debug("POST /active failed (retrying on keepalive): %s", e)


class Relay:
    def __init__(self, rx_url: str):
        self._rx_url = rx_url.rstrip("/")
        self._rx_host = urlsplit(self._rx_url).hostname or "127.0.0.1"
        self.audio_rate: int | None = None
        # Listener audio format (config `listener:` block), forwarded from
        # the rx daemon. `pcm` until the first successful refresh.
        self.listener: dict = dict(LISTENER_DEFAULT)
        self._active: dict[int, StationInfo] = {}
        self._transports: dict[int, asyncio.DatagramTransport] = {}
        self._subscribers: dict[int, set[WebSocket]] = {}
        # One bounded egress queue + drain task per PCM subscriber --
        # keyed by the websocket itself, mirroring the AAC pool's
        # per-listener shape.
        self._pcm_queues: dict[WebSocket, asyncio.Queue] = {}
        self._pcm_tasks: dict[WebSocket, asyncio.Task] = {}
        self._refresh_task: asyncio.Task | None = None
        # feedmyfm-rx's localhost status port (config `status.port`), from
        # /api/stations/active; 0 = disabled.
        self.status_port: int = 0
        self._stats: dict = {}
        self._stats_task: asyncio.Task | None = None
        self._stats_subs: set[WebSocket] = set()
        self._http: httpx.AsyncClient | None = None

        self._decode_gate = DecodeGateClient(
            get_http=lambda: self._http,
            rx_host=self._rx_host,
            get_status_port=lambda: self.status_port,
            get_active_ports=lambda: set(self._active),
            get_listener_ports=self._listener_ports,
            get_stats=lambda: self._stats,
        )
        self._aac_pool = AacEncoderPool(
            get_listener=lambda: self.listener,
            get_audio_rate=lambda: self.audio_rate,
        )

    def active_stations(self) -> dict[int, StationInfo]:
        return dict(self._active)

    def get_stats(self) -> dict:
        """Latest snapshot from the receiver's status port ({} if none yet)."""
        return self._stats

    async def start(self) -> None:
        # Default 2s covers the loopback status-port poll; the station-list
        # fetch overrides it per-request (the daemon resolves in-process).
        self._http = httpx.AsyncClient(timeout=2.0)
        await self.refresh_once()
        self._refresh_task = asyncio.create_task(self._refresh_loop())
        self._stats_task = asyncio.create_task(self._stats_loop())
        self._decode_gate.start()

    async def stop(self) -> None:
        tasks = [t for t in (self._refresh_task, self._stats_task) if t is not None]
        for task in tasks:
            task.cancel()
        if tasks:
            await asyncio.gather(*tasks, return_exceptions=True)
        # No final POST /active: relay going quiet is exactly what rx's
        # 30 s fail-open handles -- it reverts to decoding everything.
        await self._decode_gate.stop()
        self._aac_pool.shutdown()
        for task in self._pcm_tasks.values():
            task.cancel()
        self._pcm_tasks.clear()
        self._pcm_queues.clear()
        if self._http is not None:
            await self._http.aclose()
        for transport in self._transports.values():
            transport.close()
        self._transports.clear()
        for ws in list(self._stats_subs):
            try:
                await ws.close()
            except Exception:
                pass
        self._stats_subs.clear()

    async def _refresh_loop(self) -> None:
        while True:
            await asyncio.sleep(REFRESH_INTERVAL_SEC)
            try:
                await self.refresh_once()
            except Exception:
                logger.exception("Station list refresh failed, keeping previous state")

    async def _stats_loop(self) -> None:
        while True:
            await asyncio.sleep(STATS_INTERVAL_SEC)
            if not self.status_port or self._http is None:
                continue
            try:
                r = await self._http.get(f"http://{self._rx_host}:{self.status_port}/")
                r.raise_for_status()
                self._stats = r.json()
            except Exception as e:
                logger.debug("status port poll failed, keeping last: %s", e)
                continue
            await self._broadcast_stats()

    def _listener_ports(self) -> set[int]:
        """Ports with >=1 live subscriber right now (PCM websocket or AAC)."""
        ports = {p for p, s in self._subscribers.items() if s}
        ports |= self._aac_pool.active_ports()
        return ports

    async def refresh_once(self) -> None:
        if self._http is None:
            return
        try:
            r = await self._http.get(
                f"{self._rx_url}/api/stations/active", timeout=20.0
            )
            r.raise_for_status()
            result = r.json()
        except Exception as e:
            logger.error(
                f"rx daemon station list unavailable, keeping previous list: {e}"
            )
            return

        # audio_rate is None when the rx daemon can't resolve a valid plan
        # (the config is currently invalid) -- treat it like any other
        # fetch failure and hold the last good state.
        if result.get("audio_rate") is None:
            logger.error(
                "rx daemon reports no resolvable plan, keeping previous station list"
            )
            return

        self.audio_rate = result["audio_rate"]
        self.status_port = int(result.get("status_port") or 0)

        new_listener = result.get("listener") or dict(LISTENER_DEFAULT)
        codec_flip = new_listener.get("codec") != self.listener.get("codec")
        self.listener = new_listener
        if codec_flip:
            logger.info(
                "listener.codec changed to %r, disconnecting all subscribers",
                self.listener.get("codec"),
            )

        new_active = {
            s["port"]: StationInfo(
                port=s["port"],
                label=s["label"],
                freq=s["freq"],
                stereo=s["stereo"],
                stereo_mode=s.get("stereo_mode", "off"),
            )
            for s in result["stations"]
        }
        await self._apply_active_stations(new_active, codec_flip=codec_flip)

    async def _apply_active_stations(
        self, new_active: dict[int, StationInfo], *, codec_flip: bool = False
    ) -> None:
        old_ports = set(self._active)
        new_ports = set(new_active)

        # Tell open listener pages to reload rather than silently keep
        # working off a stale STATIONS list / codec assumption: a codec
        # flip otherwise strands a page in a 409 retry loop against the
        # endpoint it can no longer use, and
        # an admin add/remove otherwise never reaches pages that were
        # already open. player.js's connectStatusSocket reloads on either.
        if codec_flip:
            await self._push_to_stats_subs({"type": "codec_changed"})
        if old_ports != new_ports:
            await self._push_to_stats_subs({"type": "stations_changed"})

        # A pcm<->aac flip invalidates every open stream: the page has to
        # reconnect to the other endpoint. Drop all subscribers on all
        # ports (encoders too); the sections below then rebuild nothing
        # extra -- clients reconnect on their own.
        if codec_flip:
            for port in list(old_ports):
                for ws in self._subscribers.get(port, set()):
                    try:
                        await ws.close(
                            code=1001, reason="listener codec changed, reconnect"
                        )
                    except Exception:
                        pass
                self._subscribers[port] = set()
                self._aac_pool.drop(port)

        for port in old_ports - new_ports:
            transport = self._transports.pop(port, None)
            if transport is not None:
                transport.close()
            for ws in self._subscribers.pop(port, set()):
                try:
                    await ws.close(code=1001, reason="station no longer active")
                except Exception:
                    pass
            self._aac_pool.drop(port)
            logger.info(f"Station on port {port} is no longer active, relay stopped")

        # A station's metadata (most importantly stereo) can change on the
        # same port without the port itself being added/removed -- e.g.
        # stations.yml gets edited to flip mono<->stereo for a station
        # already streaming. stream_info (sample_rate/channels) is only
        # ever sent once, right when a websocket connects, so a client
        # that connected under the old metadata would otherwise keep
        # treating newly-stereo-interleaved bytes as mono forever,
        # producing exactly the "completely garbled" symptom this was
        # written to fix. Disconnecting existing subscribers (the UDP
        # transport/port itself is untouched -- only the format
        # description changed, not which port carries the station) forces
        # a reconnect, which gets the correct, current stream_info. The
        # AAC encoder is bound to the channel count, so it is torn down
        # here too and rebuilt (mono vs stereo) on the next listener.
        for port in old_ports & new_ports:
            if self._active[port] != new_active[port]:
                for ws in self._subscribers.get(port, set()):
                    try:
                        await ws.close(
                            code=1001, reason="station format changed, reconnect"
                        )
                    except Exception:
                        pass
                self._subscribers[port] = set()
                self._aac_pool.drop(port)
                logger.info(
                    f"Station on port {port} changed ({self._active[port]} -> "
                    f"{new_active[port]}), disconnected existing subscribers"
                )

        loop = asyncio.get_running_loop()
        for port in new_ports - old_ports:
            self._subscribers.setdefault(port, set())
            # asyncio's create_datagram_endpoint(local_addr=...) no longer
            # accepts reuse_address (removed in 3.9+ over TCP-reuse
            # security concerns that don't apply here), but a transport's
            # close() doesn't synchronously free the OS-level socket --
            # a station that briefly drops out and reappears within one
            # refresh cycle can otherwise hit "Address already in use"
            # rebinding the same port. Create the socket ourselves with
            # SO_REUSEADDR so a still-closing old socket doesn't block it.
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            try:
                sock.bind(("0.0.0.0", port))
            except OSError:
                logger.exception(f"Could not bind UDP listener for port {port}")
                sock.close()
                continue
            transport, _ = await loop.create_datagram_endpoint(
                lambda: _StationProtocol(
                    port, self._subscribers, self, self._pcm_queues
                ),
                sock=sock,
            )
            self._transports[port] = transport
            logger.info(
                f"Station on port {port} ({new_active[port].label}) is now active"
            )

        self._active = new_active
        for port in old_ports - new_ports:
            self._decode_gate.cancel_linger(port)
        self._decode_gate.kick()

    async def subscribe(self, port: int, ws: WebSocket) -> None:
        self._subscribers.setdefault(port, set()).add(ws)
        q: asyncio.Queue = asyncio.Queue(maxsize=PCM_SUB_QUEUE_MAXSIZE)
        self._pcm_queues[ws] = q
        self._pcm_tasks[ws] = asyncio.create_task(self._pcm_drain(ws, q))
        self._decode_gate.cancel_linger(port)
        self._decode_gate.kick()

    def unsubscribe(self, port: int, ws: WebSocket) -> None:
        self._subscribers.get(port, set()).discard(ws)
        self._pcm_queues.pop(ws, None)
        task = self._pcm_tasks.pop(ws, None)
        if task is not None:
            task.cancel()
        if not self._subscribers.get(port) and not self._aac_pool.has_subscribers(port):
            self._decode_gate.start_linger(port)
        self._decode_gate.kick()

    async def _pcm_drain(self, ws: WebSocket, q: asyncio.Queue) -> None:
        """One task per PCM subscriber: pulls datagrams off its own bounded
        queue and sends them, so a slow/stuck send() only ever backs up
        this subscriber's own queue, never delivery to anyone else on the
        same station. station_audio's own receive loop notices the
        disconnect and calls unsubscribe(), which cancels this task --
        just stop draining on any send failure rather than also closing
        the socket ourselves."""
        while True:
            data = await q.get()
            try:
                await ws.send_bytes(data)
            except Exception:
                return

    async def subscribe_stats(self, ws: WebSocket) -> None:
        self._stats_subs.add(ws)

    def unsubscribe_stats(self, ws: WebSocket) -> None:
        self._stats_subs.discard(ws)

    async def _broadcast_stats(self) -> None:
        await self._push_to_stats_subs({"enabled": True, **self._stats})

    async def _push_to_stats_subs(self, payload: dict) -> None:
        """Send one JSON frame to every /ws/status subscriber -- the
        periodic stats snapshot (_broadcast_stats above) and one-off
        {"type": ...} events (codec_changed, stations_changed) both ride
        this channel; player.js's connectStatusSocket tells them apart by
        the presence of "type"."""
        # Snapshot-iterate like _StationProtocol.datagram_received -- a set
        # of WebSocket mutated by a concurrent subscribe_stats()/
        # unsubscribe_stats() while we're partway through iterating it would
        # otherwise raise "Set changed size during iteration". Unlike that
        # method, this one also prunes dead subscribers itself, since a
        # failed send_text here is how a closed socket gets noticed.
        text = json.dumps(payload)
        dead = []
        for ws in list(self._stats_subs):
            try:
                await ws.send_text(text)
            except Exception:
                dead.append(ws)
        for ws in dead:
            self._stats_subs.discard(ws)

    # ---- AAC listener path ------------------------------------------------

    async def subscribe_aac(self, port: int) -> asyncio.Queue | None:
        """Register an ``audio.aac`` HTTP response. Returns its egress
        queue (already seeded with the prime backlog), or ``None`` if the
        port isn't an active station."""
        station = self._active.get(port)
        if station is None:
            return None
        q = await self._aac_pool.subscribe(station)
        self._decode_gate.cancel_linger(port)
        self._decode_gate.kick()
        return q

    def unsubscribe_aac(self, port: int, q: asyncio.Queue) -> None:
        self._aac_pool.unsubscribe(port, q)
        if not self._subscribers.get(port) and not self._aac_pool.has_subscribers(port):
            self._decode_gate.start_linger(port)
        self._decode_gate.kick()

    def _feed_encoder(self, port: int, data: bytes) -> None:
        self._aac_pool.feed(port, data)

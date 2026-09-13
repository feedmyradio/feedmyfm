import hashlib
import json
import os
import re
from pathlib import Path
from urllib.parse import parse_qs

from fastapi import APIRouter, HTTPException, Request, WebSocket, WebSocketDisconnect
from fastapi.responses import RedirectResponse, StreamingResponse
from fastapi.templating import Jinja2Templates

from webui.relay import Relay, StationInfo

router = APIRouter()
templates = Jinja2Templates(
    directory=str(Path(__file__).resolve().parent.parent / "templates")
)

# Casual shared-password gate on the listener page only -- the audio
# streams/websockets/APIs below stay open to anyone with the direct URL.
# Not meant as real access control (it's one static password for everyone,
# not per-user), just a splash screen to keep the station list off of
# search engines / drive-by visitors. Override with FEEDMYFM_SITE_PASSWORD.
SITE_PASSWORD = os.environ.get("FEEDMYFM_SITE_PASSWORD", "feedmyfm")
_SITE_COOKIE = "feedmyfm_site_auth"
_SITE_TOKEN = hashlib.sha256(SITE_PASSWORD.encode()).hexdigest()

# webui holds no config files, so station -> logo is resolved entirely from
# files in static/logos/ -- in the compose stack that dir is a read-only
# bind mount of deploy/logos/ (operator drop-in); otherwise it's just the
# bundled _default.svg. The lookup is: aliases.json override, else a slug of
# the label; first existing extension wins; nothing found falls back to the
# bundled glyph.
_LOGOS_DIR = Path(__file__).resolve().parent.parent / "static" / "logos"
_LOGO_EXTS = (".svg", ".png", ".webp", ".jpg", ".jpeg")
_DEFAULT_LOGO = "/static/logos/_default.svg"

# MediaSession artwork (aac-mode lock screen) is pinned to PNG,
# deliberately: SVG isn't accepted, JPEG/WebP add per-platform quirks, and
# a single format keeps both this lookup and the client's MediaMetadata
# trivial. A station gets its own lock-screen art by dropping <slug>.png
# in the logos dir; everything else uses the bundled 512x512 _default.png
# (a truecolor raster -- an indexed/palette PNG is rejected by iOS).
_ARTWORK_EXTS = (".png",)
_DEFAULT_ARTWORK = "/static/logos/_default.png"


def _slugify(label: str) -> str:
    return re.sub(r"[^a-z0-9]+", "-", label.lower()).strip("-") or "station"


def _load_aliases() -> dict[str, str]:
    """{label: filename-or-slug} overrides for when the slug doesn't match."""
    try:
        data = json.loads((_LOGOS_DIR / "aliases.json").read_text())
        return data if isinstance(data, dict) else {}
    except (OSError, ValueError):
        return {}


def _resolve_logo(label: str, aliases: dict[str, str]) -> str:
    stem = aliases.get(label) or _slugify(label)
    if "." in stem:  # an alias may already carry its extension
        if (_LOGOS_DIR / stem).is_file():
            return f"/static/logos/{stem}"
        stem = stem.rsplit(".", 1)[0]
    for ext in _LOGO_EXTS:
        if (_LOGOS_DIR / f"{stem}{ext}").is_file():
            return f"/static/logos/{stem}{ext}"
    return _DEFAULT_LOGO


def _resolve_artwork(label: str, aliases: dict[str, str]) -> str:
    """Same slug/alias lookup as :func:`_resolve_logo`, but PNG only, for
    ``navigator.mediaSession`` artwork. Stations without a ``<slug>.png``
    (SVG-only, or no logo at all) fall back to the bundled
    ``_default.png``."""
    stem = aliases.get(label) or _slugify(label)
    if "." in stem:
        stem = stem.rsplit(".", 1)[0]
    for ext in _ARTWORK_EXTS:
        if (_LOGOS_DIR / f"{stem}{ext}").is_file():
            return f"/static/logos/{stem}{ext}"
    return _DEFAULT_ARTWORK


def _relay(request: Request) -> Relay:
    return request.app.state.relay


def _station_dict(s: StationInfo) -> dict:
    """The {port,label,freq,stereo,stereo_mode} core every station JSON
    object here carries -- shared by index() and active_stations().
    index() layers logo/artwork on top."""
    return {
        "port": s.port,
        "label": s.label,
        "freq": s.freq,
        "stereo": s.stereo,
        "stereo_mode": s.stereo_mode,
    }


@router.get("/login")
def login_page(request: Request, error: bool = False):
    if request.cookies.get(_SITE_COOKIE) == _SITE_TOKEN:
        return RedirectResponse("/")
    return templates.TemplateResponse(request, "splash.html", {"error": error})


@router.post("/login")
async def login_submit(request: Request):
    # Plain url-encoded form body, parsed by hand rather than via FastAPI's
    # Form(...) (which pulls in python-multipart just to declare the
    # parameter, even for this simple non-multipart body).
    body = parse_qs((await request.body()).decode())
    password = (body.get("password") or [""])[0]
    if hashlib.sha256(password.encode()).hexdigest() != _SITE_TOKEN:
        return RedirectResponse("/login?error=1", status_code=303)
    response = RedirectResponse("/", status_code=303)
    response.set_cookie(
        _SITE_COOKIE,
        _SITE_TOKEN,
        max_age=180 * 24 * 3600,
        httponly=True,
        samesite="lax",
        secure=True,
    )
    return response


@router.get("/")
def index(request: Request):
    if request.cookies.get(_SITE_COOKIE) != _SITE_TOKEN:
        return RedirectResponse("/login")
    relay = _relay(request)
    aliases = _load_aliases()
    stations = [
        {
            **_station_dict(s),
            "logo": _resolve_logo(s.label, aliases),
            "artwork": _resolve_artwork(s.label, aliases),
        }
        for s in sorted(relay.active_stations().values(), key=lambda s: s.freq)
    ]
    return templates.TemplateResponse(
        request,
        "index.html",
        {"stations": stations, "listener": relay.listener},
    )


@router.get("/api/status")
def receiver_status(request: Request):
    """Latest signal metadata from feedmyfm-rx's status port, proxied
    server-side (the browser can't reach the receiver's loopback)."""
    relay = _relay(request)
    stats = relay.get_stats()
    if not stats:
        return {"enabled": bool(relay.status_port), "sdr": None, "stations": []}
    return {"enabled": True, **stats}


@router.get("/api/stations/active")
def active_stations(request: Request):
    relay = _relay(request)
    stations = sorted(relay.active_stations().values(), key=lambda s: s.freq)
    return {
        "audio_rate": relay.audio_rate,
        "listener": relay.listener,
        "stations": [_station_dict(s) for s in stations],
    }


@router.get("/stations/{port}/audio.aac")
async def station_audio_aac(request: Request, port: int):
    """Endless chunked ``audio/aac`` (ADTS) stream for one station, for
    the ``<audio>`` element in ``listener.codec: aac`` mode. The encoder
    is built on the first connection to a station and torn down with the
    last (see :meth:`Relay.subscribe_aac`)."""
    relay = _relay(request)
    if port not in relay.active_stations():
        raise HTTPException(status_code=404, detail="station not active")
    if relay.audio_rate is None:
        # rx's plan hasn't resolved yet -- subscribe_aac() would otherwise
        # build the encoder at AAC_RATE as a guess, not the station's real
        # rate. Same window station_audio (pcm ws) below guards against.
        raise HTTPException(
            status_code=503, detail="station list not ready yet, retry shortly"
        )
    if relay.listener.get("codec") != "aac":
        # Belt-and-braces: the page won't request this in pcm mode.
        raise HTTPException(status_code=409, detail="listener codec is not aac")

    queue = await relay.subscribe_aac(port)
    if queue is None:
        raise HTTPException(status_code=404, detail="station not active")

    async def gen():
        try:
            while True:
                frame = await queue.get()
                if frame is None:  # station gone / format flip / codec flip
                    break
                yield frame
        finally:
            relay.unsubscribe_aac(port, queue)

    return StreamingResponse(
        gen(),
        media_type="audio/aac",
        headers={"Cache-Control": "no-store"},
    )


@router.websocket("/ws/stations/{port}/audio")
async def station_audio(websocket: WebSocket, port: int):
    relay: Relay = websocket.app.state.relay
    stations = relay.active_stations()
    station = stations.get(port)
    if station is None:
        await websocket.close(code=1008, reason="station not active")
        return
    if relay.audio_rate is None:
        # rx's plan hasn't resolved yet -- AudioContext({sampleRate: null})
        # throws client-side. 1013 = "try again later".
        await websocket.close(code=1013, reason="station list not ready yet")
        return

    await websocket.accept()
    await websocket.send_text(
        json.dumps(
            {
                "type": "stream_info",
                "sample_rate": relay.audio_rate,
                "channels": 2 if station.stereo else 1,
                "format": "s16le",
            }
        )
    )
    await relay.subscribe(port, websocket)
    try:
        while True:
            # This websocket is push-only (server -> client); awaiting a
            # message here just blocks until the client disconnects,
            # without spinning the CPU. Must be receive_text()/
            # receive_bytes() (which raise WebSocketDisconnect via
            # _raise_on_disconnect), not the raw receive() -- that
            # returns the disconnect message as a plain dict instead of
            # raising, so a naive loop calls it again afterward and hits
            # Starlette's "Cannot call receive once a disconnect message
            # has been received" RuntimeError instead of exiting cleanly.
            await websocket.receive_text()
    except WebSocketDisconnect:
        pass
    finally:
        relay.unsubscribe(port, websocket)


@router.websocket("/ws/status")
async def status_stream(websocket: WebSocket):
    """Push-based replacement for polling ``/api/status``: sends the
    current signal-metadata snapshot on connect, then one more message
    each time ``Relay._stats_loop`` refreshes it (see
    :meth:`Relay._broadcast_stats`)."""
    relay: Relay = websocket.app.state.relay
    await websocket.accept()
    stats = relay.get_stats()
    await websocket.send_text(
        json.dumps(
            {"enabled": True, **stats}
            if stats
            else {"enabled": bool(relay.status_port), "sdr": None, "stations": []}
        )
    )
    await relay.subscribe_stats(websocket)
    try:
        while True:
            # Push-only, same as station_audio above: block until disconnect.
            await websocket.receive_text()
    except WebSocketDisconnect:
        pass
    finally:
        relay.unsubscribe_stats(websocket)

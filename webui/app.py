"""
FastAPI app factory for the feedmyfm listener web UI. Separated from
main.py (the Typer CLI entrypoint) so tests can construct an app against
an arbitrary feedmyfm-rx daemon URL without going through the CLI.
"""

from contextlib import asynccontextmanager
from pathlib import Path

from fastapi import FastAPI
from fastapi.staticfiles import StaticFiles

from webui.relay import Relay
from webui.routes.admin import router as admin_router
from webui.routes.listener import router as listener_router

STATIC_DIR = Path(__file__).resolve().parent / "static"


def create_app(rx_url: str) -> FastAPI:
    """``rx_url``: base URL of the feedmyfm-rx daemon's HTTP port. It is
    the source of both the active-station plan (listener side, via the
    relay) and the config editor (``/admin`` proxy)."""
    rx_url = rx_url.rstrip("/")
    relay = Relay(rx_url)

    @asynccontextmanager
    async def lifespan(app: FastAPI):
        await relay.start()
        yield
        await relay.stop()

    app = FastAPI(title="feedmyfm listener", lifespan=lifespan)
    app.state.relay = relay
    app.state.rx_url = rx_url

    @app.middleware("http")
    async def _cache_static(request, call_next):
        # Logos / artwork are stable content-addressable-ish files: cache
        # them hard so a reload or station switch doesn't re-fetch the
        # lock-screen image. Code (js/css) must stay fresh -- `no-cache`
        # keeps it in the cache but forces an etag revalidation (a quick
        # 304) every load. StaticFiles sets no Cache-Control by default.
        path = request.url.path
        resp = await call_next(request)
        if path.startswith(("/static/logos/", "/static/img/")):
            resp.headers.setdefault("Cache-Control", "public, max-age=86400")
        else:
            resp.headers.setdefault("Cache-Control", "no-cache")
        return resp

    app.include_router(listener_router)
    app.include_router(admin_router)
    app.mount("/static", StaticFiles(directory=str(STATIC_DIR)), name="static")
    return app

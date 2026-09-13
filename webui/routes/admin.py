"""
Admin page + a transparent proxy to the feedmyfm-rx daemon.

The browser only ever talks to webui -- admin requests get forwarded to
RX_URL via httpx, passing the incoming Authorization header through
unmodified. That means webui never needs its own copy of the password
(the daemon is the only place that checks it). It also keeps the
daemon's admin port reachable only from wherever webui runs, not from
arbitrary browsers, when the two are on different machines.

The daemon's WWW-Authenticate response header is deliberately *not*
forwarded (see _HOP_BY_HOP_HEADERS) -- admin.js has its own login form
and handles a 401 itself, and letting that header through just left
the browser's own native Basic-Auth dialog free to pop up alongside it.
"""

import logging
from pathlib import Path

import httpx
from fastapi import APIRouter, Request, Response
from fastapi.templating import Jinja2Templates

logger = logging.getLogger(__name__)

router = APIRouter()
templates = Jinja2Templates(
    directory=str(Path(__file__).resolve().parent.parent / "templates")
)

# Headers that are specific to the hop-by-hop connection itself, or that
# uvicorn/Starlette will set on webui's own response regardless -- must
# not be blindly forwarded from the upstream (rx daemon) response, or
# they end up duplicated (e.g. two "date"/"server" header lines).
#
# WWW-Authenticate is dropped for a different reason: admin.js owns the
# login UI (a real form, not the browser's native Basic-Auth dialog) and
# handles 401s itself. Forwarding this header would leave that native
# dialog free to pop up too, inconsistently across browsers, racing the
# page's own form.
_HOP_BY_HOP_HEADERS = {
    "content-length",
    "transfer-encoding",
    "connection",
    "date",
    "server",
    "www-authenticate",
}


@router.get("/admin")
def admin_page(request: Request):
    return templates.TemplateResponse(request, "admin.html", {})


@router.api_route("/admin/api/{path:path}", methods=["GET", "POST", "PUT"])
async def proxy_to_rx(path: str, request: Request):
    rx_url = request.app.state.rx_url
    if not rx_url:
        return Response(
            content='{"detail": "RX_URL is not configured for this webui instance"}',
            status_code=503,
            media_type="application/json",
        )

    body = await request.body()
    headers = {k: v for k, v in request.headers.items() if k.lower() != "host"}

    # A band scan blocks the rx response for its whole accumulation window
    # (config scan.average_ms, up to 15 s) plus a little slack -- the
    # config/stations GET+PUT are all sub-second, so one generous timeout
    # covers both rather than special-casing the path.
    async with httpx.AsyncClient() as client:
        upstream = await client.request(
            request.method,
            f"{rx_url}/api/{path}",
            headers=headers,
            content=body,
            timeout=20.0,
        )

    # A successful save changes the active-station plan. The relay would
    # pick it up on its next poll (REFRESH_INTERVAL_SEC); pull it forward
    # so port binds/unbinds happen at save time. The daemon serves the
    # new plan the instant the PUT returns (it re-reads the files per
    # request), so there's no race. Best-effort -- a refresh hiccup must
    # not turn a successful save into an error.
    if (
        request.method == "PUT"
        and path.endswith("/raw")
        and upstream.status_code == 200
    ):
        try:
            await request.app.state.relay.refresh_once()
        except Exception:  # noqa: BLE001 -- logged, save still succeeded
            logger.warning(
                "post-save relay refresh failed; will catch up on the next poll",
                exc_info=True,
            )

    response_headers = {
        k: v
        for k, v in upstream.headers.items()
        if k.lower() not in _HOP_BY_HOP_HEADERS
    }
    return Response(
        content=upstream.content,
        status_code=upstream.status_code,
        headers=response_headers,
    )

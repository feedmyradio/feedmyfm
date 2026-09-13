#!/usr/bin/env python3
"""
webui: a browser listener page for feedmyfm's active stations.

Gets its active-station list from the feedmyfm-rx daemon over HTTP
(--rx-url), polled on a timer (see relay.py) -- no config files
on this side. Can run on the same machine as feedmyfm-rx or a separate
one; if separate, config.yml's udp.host must point at wherever this runs
so the station audio arrives here, and the daemon's status.host must be
reachable from here.
"""

import typer
import uvicorn

from webui.app import create_app

app = typer.Typer(
    name="feedmyfm-webui",
    help="Browser listener UI for feedmyfm's active stations",
    add_completion=False,
)


@app.command()
def main(
    host: str = typer.Option(
        "0.0.0.0", "--host", help="Address to bind the web server to"
    ),
    port: int = typer.Option(8080, "--port", help="Port to bind the web server to"),
    rx_url: str = typer.Option(
        "http://127.0.0.1:8082",
        "--rx-url",
        envvar="RX_URL",
        help="Base URL of the feedmyfm-rx daemon's HTTP port (config "
        "status.host/status.port) -- the active-station plan and the "
        "/admin config editor both come from it "
        "(e.g. http://sourcing-workstation:8082).",
    ),
):
    """Start the feedmyfm listener web UI."""
    fastapi_app = create_app(rx_url)
    uvicorn.run(fastapi_app, host=host, port=port)


if __name__ == "__main__":
    app()

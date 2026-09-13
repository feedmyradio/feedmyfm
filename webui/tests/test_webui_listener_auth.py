"""Tests for the listener page's shared-password splash gate
(webui/routes/listener.py login_page/login_submit/index). The audio
streams/APIs are deliberately not gated -- only "/" is."""

import pytest

# webui.relay -> webui.aac_encoder -> PyAV (a `webui` extra). Skip cleanly
# when it isn't installed; this module only tests the login splash gate.
pytest.importorskip("av")

from fastapi.testclient import TestClient  # noqa: E402

from webui.app import create_app  # noqa: E402
from webui.relay import Relay  # noqa: E402
from webui.routes.listener import SITE_PASSWORD  # noqa: E402


def _client():
    app = create_app("http://rx.test")
    relay = Relay("http://rx.test")
    relay.audio_rate = 32_000
    relay._active = {}
    app.state.relay = relay
    # https base_url: the cookie is Secure (the real site is only ever
    # reached over HTTPS, via the operator's HTTPS front end), so a plain
    # http TestClient would silently drop it on the next request, same as
    # a real browser would.
    return TestClient(
        app, base_url="https://testserver", follow_redirects=False
    )  # no `with`: skip lifespan


def test_index_redirects_to_login_without_cookie():
    client = _client()
    resp = client.get("/")
    assert resp.status_code in (302, 303, 307)
    assert resp.headers["location"] == "/login"


def test_login_page_renders():
    client = _client()
    resp = client.get("/login")
    assert resp.status_code == 200
    assert "splash-form" in resp.text
    assert "Incorrect password" not in resp.text


def test_wrong_password_redirects_back_with_error():
    client = _client()
    resp = client.post("/login", data={"password": "not-it"})
    assert resp.status_code == 303
    assert resp.headers["location"] == "/login?error=1"
    assert "feedmyfm_site_auth" not in resp.headers.get("set-cookie", "")

    error_page = client.get("/login?error=1")
    assert "Incorrect password" in error_page.text


def test_correct_password_sets_cookie_and_unlocks_index():
    client = _client()
    resp = client.post("/login", data={"password": SITE_PASSWORD})
    assert resp.status_code == 303
    assert resp.headers["location"] == "/"
    assert "feedmyfm_site_auth" in resp.headers.get("set-cookie", "")

    index_resp = client.get("/")
    assert index_resp.status_code == 200
    assert "No stations are currently active" in index_resp.text

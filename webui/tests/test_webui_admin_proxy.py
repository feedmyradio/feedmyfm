"""The /admin/api/{path} proxy (webui/routes/admin.py) forwards any
subpath to the rx daemon's /api/{path}, passing the Authorization header
through unchanged. Covers the GET /api/status/full route the /admin
Signal tab uses -- it needs no special-casing
in webui, just the generic passthrough."""

import pytest

# webui.app -> webui.relay -> PyAV. Skip cleanly when it isn't installed.
pytest.importorskip("av")

from fastapi.testclient import TestClient  # noqa: E402

from webui.app import create_app  # noqa: E402


class _FakeResponse:
    def __init__(self, status_code=200, content=b'{"stations": []}', headers=None):
        self.status_code = status_code
        self.content = content
        self.headers = headers or {"content-type": "application/json"}


class _FakeAsyncClient:
    """Records the last outbound request and returns a canned 200."""

    last: dict = {}

    def __init__(self, *args, **kwargs):
        pass

    async def __aenter__(self):
        return self

    async def __aexit__(self, *exc):
        return False

    async def request(self, method, url, headers=None, content=None, timeout=None):
        _FakeAsyncClient.last = {
            "method": method,
            "url": url,
            "headers": headers or {},
            "content": content,
        }
        return _FakeResponse()


def test_proxy_forwards_status_full_with_auth(monkeypatch):
    _FakeAsyncClient.last = {}
    monkeypatch.setattr("webui.routes.admin.httpx.AsyncClient", _FakeAsyncClient)
    client = TestClient(create_app("http://rx.test/"))  # trailing / is stripped

    resp = client.get(
        "/admin/api/status/full", headers={"Authorization": "Basic dXNlcjpwdw=="}
    )

    assert resp.status_code == 200
    assert _FakeAsyncClient.last["method"] == "GET"
    assert _FakeAsyncClient.last["url"] == "http://rx.test/api/status/full"
    assert _FakeAsyncClient.last["headers"]["authorization"] == "Basic dXNlcjpwdw=="
    # host header must not be forwarded verbatim (routes/admin.py drops it)
    assert "host" not in _FakeAsyncClient.last["headers"]


def test_proxy_503_when_no_rx_url(monkeypatch):
    _FakeAsyncClient.last = {}
    monkeypatch.setattr("webui.routes.admin.httpx.AsyncClient", _FakeAsyncClient)
    client = TestClient(create_app(""))

    resp = client.get("/admin/api/status/full")

    assert resp.status_code == 503
    assert _FakeAsyncClient.last == {}  # never reached the upstream request

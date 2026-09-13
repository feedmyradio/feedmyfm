# webui: the listener UI. Pure Python (FastAPI). It gets the
# active-station plan from the feedmyfm-rx daemon over HTTP (RX_URL)
# -- no config files and no feedmyfm-rx binary on this side. Build
# context is the repo root.

FROM python:3.12-slim
ENV DEBIAN_FRONTEND=noninteractive

WORKDIR /app

COPY --from=ghcr.io/astral-sh/uv:0.8.0 /uv /usr/local/bin/uv

COPY webui/pyproject.toml webui/uv.lock README.md ./
COPY webui ./webui

# The webui extra + this project.
RUN uv export --no-hashes --no-dev --extra webui --format requirements-txt -o /tmp/requirements.txt \
    && uv pip install --system --break-system-packages --no-cache -r /tmp/requirements.txt \
    && rm /tmp/requirements.txt /usr/local/bin/uv

ENTRYPOINT ["feedmyfm-webui"]

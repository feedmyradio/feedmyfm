# FeedMyFM

[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)

Receive **every FM station in the band from one SDR at once** and stream each
one as raw PCM over its own UDP port. One capture, one channelizer, one
process; N stations out.

The receiver is **`feedmyfm-rx`** (`rx/`) — a standalone C++
program built on [liquid-dsp](https://liquidsdr.org/) and plain `libiio`: a
single native binary, no runtime interpreter. It reads `config.yml` +
`stations.yml` directly and does its own frequency planning and validation.
An optional Python `webui` service adds a browser listener page and a
config editor (both fed by the daemon's own HTTP port), but the receiver
never depends on it.

---

## Contents

- [How it works](#how-it-works)
- [Requirements](#requirements)
- [Installation](#installation)
- [Quick start](#quick-start)
- [Listening to a stream](#listening-to-a-stream)
- [Configuration](#configuration)
  - [`config.yml` reference](#configyml-reference)
  - [`stations.yml` reference](#stationsyml-reference)
  - [Validation: `--check`](#validation---check)
  - [Retuning `center_freq`](#retuning-center_freq)
- [Stereo decoding](#stereo-decoding)
- [AGC and squelch](#agc-and-squelch)
- [Status port](#status-port)
- [The web stack](#the-web-stack)
- [Operations](#operations)
- [Development](#development)
- [Troubleshooting](#troubleshooting)
- [Further reading](#further-reading)
- [Legal](#legal)
- [License](#license)

---

## How it works

```
              ┌───────────────── feedmyfm-rx (one process) ─────────────────┐
 PlutoSDR ──► │  libiio  ──►  polyphase channelizer (firpfbchr, FFT-bound)  │
  (IIO/IP)    │                          │                                  │
              │              one output "bin" per FFT slice                 │
              │            ┌─────────────┼─────────────┐                    │
              │       station A      station B      station C   ... (pool)  │
              │       fine mix        fine mix        fine mix              │
              │       pre-demod LPF   pre-demod LPF   pre-demod LPF         │
              │       FM discriminator …              …                     │
              │       de-emphasis     de-emphasis     de-emphasis           │
              │       (or stereo MPX decode, per station)                   │
              │       audio decimate  audio decimate  audio decimate        │
              │       AGC / squelch   AGC / squelch   AGC / squelch         │
              │       int16 + UDP     int16 + UDP     int16 + UDP           │
              └──────────┼──────────────┼──────────────┼────────────────────┘
                    udp :7355      udp :7356      udp :7357   → any consumer
```

- **One SDR capture** at `radio.samp_rate` (which is also the channelizer
  input rate — there is no separate DDC or processing rate).
- **A polyphase channelizer** (`firpfbchr_crcf`) splits the whole captured
  span into `num_channels` equal bins. With `oversample: 2` each bin comes
  out at `2 × bin_width` so a station near a bin edge still has its full
  Carson bandwidth.
- Each configured station is assigned the **nearest bin**, then a small
  **fine mixer** shift brings it exactly to DC. A **worker pool** runs the
  per-station chains across CPU cores.
- Per station the chain is: pre-demod low-pass → FM discriminator →
  de-emphasis → audio decimation to `audio.rate` → optional AGC → optional
  squelch → `s16le` → one UDP datagram per `udp.mtu` bytes. Stations marked
  `stereo` additionally decode the 19 kHz pilot and the 23–53 kHz L−R
  subcarrier (see [Stereo decoding](#stereo-decoding)).
- **All frequency planning and config validation** live in one place:
  `rx/src/resolve.hpp`. `feedmyfm-rx --check` runs exactly
  that and prints the result. The Python tools shell out to it; they never
  re-implement any of the math.

### Repository layout

| Path | What |
|---|---|
| `rx/` | The C++ receiver. `src/resolve.hpp` = planning + validation; `src/yaml.hpp` = a ~250-line vendored YAML subset parser; `src/mono_station.hpp` / `src/stereo_decoder.hpp` / `src/rds_decoder.hpp` / `src/dsp_util.hpp` = the per-station DSP. Also carries the offline tools: `--check`, `--scan`, `--optimize-cf`, the DSP self-tests. |
| `webui/` | Optional FastAPI service: the browser listener page + a UDP→WebSocket relay, plus the `/admin` console — a tabbed station-list / server-config editor (structured fields client-side, `js-yaml`) and the band scan, all a transparent proxy to the daemon. Holds no config files; reads the active-station plan and proxies edits via the daemon's HTTP port. The only Python left in the tree. |
| `deploy/` | Everything for running it: `compose.rx.yml` / `compose.web.yml`, `.env.example`. |

---

## Requirements

**Hardware**

- **SDR**: PlutoSDR, or any `libiio`-compatible device reachable over IP or USB.
- **Antenna** for the FM broadcast band (≈ 88–108 MHz).
- **CPU**: enough headroom for real-time channelizing + N demod chains. The
  reference config (20.48 MHz span, 64 channels, 16 stations) sustains in
  real time on a ~16-core desktop. Fewer/narrower stations need far less.

**Software**

- Linux (the UDP + `libiio` networking model assumes it; Docker images are
  Linux-only and need `network_mode: host`).
- To build from source: `build-essential`, `cmake`, `pkg-config`,
  `libliquid-dev`, `libiio-dev`.
- For the optional web UI: Docker, or Python ≥ 3.12 with the `webui`
  extra.

---

## Installation

### Docker (recommended for the receiver)

```bash
cp deploy/config.yml.example deploy/config.yml
cp deploy/stations.yml.example deploy/stations.yml
$EDITOR deploy/config.yml deploy/stations.yml   # at minimum: sdr.ip, your stations
docker compose -f deploy/compose.rx.yml up -d --build
```

The image is a slim Debian with just `libiio` and the distro's FFTW-backed
`libliquid` (~350 MB). `config.yml` / `stations.yml` are bind-mounted
read-only; the compose file passes `--watch` so edits take effect without a
manual restart. `network_mode: host` is required so the container's UDP
streams reach host-side listeners and it can talk to the SDR.

The build ISA defaults to `x86-64-v3` (AVX2+FMA, ~2015+). For an image pinned
to the build host, or for older hardware:

```bash
# in deploy/compose.rx.yml, service rx:
#   build.args.MARCH: native      # or x86-64-v2, etc.
```

### From source

```bash
sudo apt install build-essential cmake pkg-config libliquid-dev libiio-dev
git clone https://github.com/yourusername/feedmyfm.git
cd feedmyfm
make build-rx        # -> rx/build/feedmyfm-rx
```

`make build-rx` builds with `-O3 -march=native`. To cross-target, pass
`-DFMRX_MARCH=…` to the `cmake -S rx -B …` step. If liquid-dsp
is in a non-standard prefix, set `LIQUID_ROOT` (or `LIQUID_INCLUDE_DIR` /
`LIQUID_LIBRARY`).

> **On liquid-dsp version:** use Debian's packaged `libliquid` (1.5.x). The
> multichannel filterbank (`firpfbchr`) regressed ~2× in speed after 1.5, so
> a newer from-source build is *slower*. Only build from source if you pin an
> old release.

### The web stack

```bash
pip install -e ".[webui]"       # listener UI + /admin editor -> feedmyfm-webui
```

or use the compose files (see [The web stack](#the-web-stack)). `webui`
talks only to the `feedmyfm-rx` daemon over HTTP (`RX_URL`)
for validation — the compose images build it in.

---

## Quick start

```bash
feedmyfm-rx -c deploy/config.yml -s deploy/stations.yml            # live, from the SDR
feedmyfm-rx -c deploy/config.yml -s deploy/stations.yml --check    # validate config and exit
feedmyfm-rx -c deploy/config.yml -s deploy/stations.yml --check --json   # machine-readable
feedmyfm-rx -c deploy/config.yml -s deploy/stations.yml --selftest # synthetic signal, no SDR
```

`--check` prints the derived rates, every station's assigned bin and fine-mix
offset, and every non-fatal finding; it exits non-zero on a hard error. Run it
after every config edit. In Docker: `docker exec feedmyfm-rx feedmyfm-rx -c
/config/config.yml -s /config/stations.yml --check`.

Full CLI:

```
feedmyfm-rx [-c deploy/config.yml] [-s deploy/stations.yml]
            [--check [--json] | --selftest | --stereo-selftest | --sweep |
             --snr-cal | --demod-cal | --scan]
            [--watch] [--seconds N] [--proto-m M] [--threads N] [--verbose]
```

| Flag | Purpose |
|---|---|
| `-c`, `--config` | Path to `config.yml` (default `/config/config.yml`). |
| `-s`, `--stations` | Path to `stations.yml` (default `/config/stations.yml`). |
| `--check` | Resolve + validate the plan, print it, exit. Add `--json` for a machine-readable dump. |
| `--selftest` | Push a synthetic two-tone FM signal through the real DSP, report tone levels + SINAD. No SDR. |
| `--stereo-selftest` | Synthetic FM stereo multiplex through the real channelizer + stereo decoder; report pilot lock and L/R separation. No SDR. |
| `--sweep` | Print the end-to-end audio frequency response vs the ideal de-emphasis curve. No SDR. |
| `--snr-cal` | Sweep carrier-to-noise ratio through the synthetic chain; tabulate the calibrated `snr_db` against the true recovered-audio SNR and check they track (fits/validates `kSnrKNoiseDefault`). No SDR. |
| `--demod-cal` | Sweep the FM threshold knee for `discriminator` / `+declick` / `pll` and tabulate the true recovered-audio SNR of each — the per-baseline go/no-go for `fm.declick` / `fm.demod: pll`. No SDR. |
| `--scan` | Run the passive band-scan periodogram (see [`scan:`](#configyml-reference)) on a synthetic multiplex and print the detected carriers. No SDR. |
| `--seconds N` | Duration for the self-test modes. |
| `--watch` | Re-exec the receiver when `config.yml` / `stations.yml` changes and still resolves (~1–3 s audio gap; a bad edit is logged and ignored). |
| `--threads N` | Cap the per-station worker pool (default: one per core). Lower it on a busy host. |
| `--proto-m M` | Override `channelizer.proto_semilen_m` (perf experiments only). |
| `--verbose` | Extra logging. |

---

## Listening to a stream

Each station streams **raw interleaved `s16le` PCM** at `audio.rate` — mono
(1 channel) by default, or 2-channel interleaved L/R for a `stereo` station.
There is no container and no header; point any raw-PCM consumer at the port.

```bash
# ffplay  (mono station, audio.rate = 32000)
ffplay -nodisp -f s16le -ar 32000 -ac 1 udp://localhost:7355

# ffplay  (stereo station)
ffplay -nodisp -f s16le -ar 32000 -ac 2 udp://localhost:7357

# VLC
vlc --demux=rawaud --rawaud-channels 1 --rawaud-samplerate 32000 \
    --rawaud-fourcc s16l udp://@:7355

# sox play (needs endianness + rate + channels spelled out)
socat -u UDP-RECV:7355 - | play -t raw -r 32000 -e signed -b 16 -c 1 -

# record to WAV
ffmpeg -f s16le -ar 32000 -ac 1 -i udp://localhost:7355 -t 60 out.wav
```

Every station must set its own `port:` in `stations.yml` -- there is no
auto-assignment. `feedmyfm-rx --check` prints the final assignment.

> **`udp.host`:** `0.0.0.0` means the receiver sends to `127.0.0.1` — only
> listeners **on the same host** receive audio. To stream to another machine,
> set `udp.host` to that machine's address. (The web relay is the usual way to
> get audio to browsers on other machines without opening the port range.)

---

## Configuration

Two files. `config.yml` is the receiver + SDR + DSP setup; `stations.yml` is
the list of stations. Frequencies and rates are plain integers in Hz
(`94200000`); `_` digit separators (`94_200_000`) are still accepted for
back-compat but no longer used in the shipped files.
Unknown or renamed keys are a **hard error** — there is no silent fallback.

Copy the `.example` files to start:

```bash
cp deploy/config.yml.example deploy/config.yml
cp deploy/stations.yml.example deploy/stations.yml
```

### `config.yml` reference

```yaml
sdr:
  ip: 10.0.0.10          # PlutoSDR address; becomes the IIO URI "ip:<this>"
  gain_db: 23             # RX gain, 0–73 dB. In manual mode this is the fixed
                          # gain; in an AGC mode it is the startup value, and
                          # under auto_sw it is also the ceiling. Chosen by
                          # measuring raw ADC headroom — the 12-bit ADC should
                          # use a healthy fraction of its range without
                          # clipping. Re-measure after an antenna, location, or
                          # hardware change.
  gain_mode: manual       # optional. manual | slow_attack | fast_attack |
                          # hybrid | auto_sw
                          #   manual      fixed gain_db, as before (default)
                          #   slow/fast/hybrid  the AD9361's own AGC on the
                          #     whole-band composite; hybrid = clip-protection
                          #     only. Takes effect on receiver restart.
                          #   auto_sw     chip stays in manual; a software loop
                          #     regulates the composite peak to a headroom
                          #     target (tracks propagation/weather without the
                          #     "one strong signal pumps every station" of the
                          #     chip AGC).
  agc:                    # optional; only read (and only accepted) under
                          # gain_mode: auto_sw. All sub-keys optional.
    target_dbfs: -9       # regulate the composite peak EMA to here
    max_dbfs: -3          # peak above this steps the gain down immediately
    min_gain_db: 0        # lower clamp (upper clamp is sdr.gain_db)
    step_db: 1            # gain change per adjustment
    interval_ms: 2000     # min spacing between step-ups / soft step-downs
    hysteresis_db: 3      # step up only when the peak is this far below target
    clip_ppm_trip: 5      # clipped-sample rate forcing an immediate step down

radio:
  center_freq: 100306000    # SDR tune frequency, Hz. Also the channelizer's
                            # centre. Stations outside center_freq ± samp_rate/2
                            # are dropped (with a note), not an error.
  samp_rate: 20480000       # SDR sample rate AND channelizer input rate, Hz.

channelizer:
  num_channels: 64        # power of 2. bin_width = samp_rate / num_channels.
  oversample: 2           # must divide num_channels. Each bin runs at
                          # bin_width × oversample, giving guard room at bin edges.
  atten_db: 60            # prototype-filter stop-band attenuation. Higher =
                          # more taps = more CPU = cleaner adjacent rejection.
  proto_semilen_m: 12     # prototype semi-length (taps = 2·M·m). Use 8 for
                          # Ubuntu's older libliquid 1.3.2.
  block_size: 131072      # IIO buffer size in samples; must be a multiple of
                          # num_channels / oversample.

fm:
  deviation: 75000        # peak FM deviation, Hz (broadcast = 75 kHz)
  tau: 50e-6              # de-emphasis time constant (50 µs EU / 75 µs US)
  intermediate_rate: 64000    # rate after the first audio decimation stage;
                              # must be reachable from the channel rate by
                              # integer decimation, and ≥ 2 × audio.bandwidth.
  predemod_transition: 20000  # pre-demod filter transition width. Also the
                              # guard band added on top of Carson bandwidth in
                              # the "does this station fit its bin" check.
  # demod: discriminator (default) | pll -- pll is the experimental
  #   threshold-extension demod from `--demod-cal`, ~1-2 dB better
  #   weak-signal SNR; mono stations only (forced back otherwise).
  # declick: false (default) | true -- suppress discriminator 2π phase-slip
  #   clicks near threshold; suppression rate is `declick_ppm` on the
  #   status port.
  # declick_sigma: 5.0 -- click detection threshold, must be > 1.0.
  # pll_bw_hz: 0 -- pll demod loop bandwidth, Hz; only used with demod: pll.
  # afc: true (default) | false -- carrier-drift tracking; false pins the
  #   fine mixer static, for a signal too multipath-corrupted to track.
  # limiter: true (default) | false -- look-ahead limiter; false falls back
  #   to the tanh soft-clip.

audio:
  rate: 32000            # output PCM sample rate, Hz. Must divide intermediate_rate.
  bandwidth: 15000       # audio passband edge, Hz (must be < rate / 2)
  stop_bandwidth: 19000  # audio stop-band edge, Hz (must be > bandwidth)

udp:
  host: 0.0.0.0           # datagram destination. 0.0.0.0 ⇒ 127.0.0.1 (local only).
  mtu: 1316               # bytes per datagram (1–65507). 1316 fits a 1500-byte
                          # path with headroom for IP/UDP + tunneling overhead.

agc:                      # soft output leveling (on by default)
  enabled: true
  target: 0.5             # output RMS target, 0–1
  max_gain_db: 40         # gain cap, so a dead channel isn't amplified to hiss
  response_ms: 200        # attack/release time constant

squelch:                  # opt-in; mutes on the ultrasonic-noise-ratio metric
  enabled: false
  open_snr_db: 15         # threshold to open; tune from the status port's snr_db
  hang_ms: 800            # hold-open time after the metric drops

stereo:                   # applies to stations with `stereo: true` / `stereo: auto`
  mode: auto              # global default decode mode: false (default, mono) |
                          # true | auto; a per-station `stereo:` in
                          # stations.yml overrides it
  pilot_threshold_db: -30 # 19 kHz pilot power (dBc of composite) to lock stereo
  blend_snr_lo_db: 12     # `auto`: full mono at/below this composite snr_db
  blend_snr_hi_db: 22     # `auto`: full stereo at/above this composite snr_db
  pilot_pll: true         # false: normalise-and-square carrier recovery instead
                          # of a PLL locked to the 19 kHz pilot

snr:                      # status-port snr_db readout
  # k_noise: 2.278        # optional: override the compiled scaling constant
                          # (from `feedmyfm-rx --snr-cal`) instead of rebuilding.
                          # snr_db is always the calibrated, physically-
                          # referenced, program-independent SNR (guard-band
                          # noise-PSD estimate, ~2 elliptic IIRs per station at
                          # channel_rate) -- there is no opt-out. A legacy
                          # `calibrated:` sub-key or bare `snr: <bool>` is still
                          # accepted but ignored (with a `--check` deprecation
                          # note); squelch_metric_db always carries the raw
                          # proxy regardless.

rds:                      # RDS (Radio Data System) decode. Off by default.
  enabled: true            # global default; a per-station `rds:` in
                          # stations.yml overrides it (same true/false/mapping
                          # pattern as agc/squelch). Only actually runs where
                          # channel_rate >= 120 kHz and -- under
                          # decode.listener_gated -- the station is active.
                          # Also accepted as a bare bool: `rds: true`.

listener:                 # audio format webui hands the browser; validated
                          # here so this stays the one authoritative surface,
                          # even though only webui consumes it. The UDP
                          # output from feedmyfm-rx is byte-for-byte
                          # identical either way. A config reload (restart or
                          # `--watch`) is needed for a change to reach webui's
                          # relay via GET /api/stations/active.
  codec: pcm               # pcm (default) | aac. pcm: raw s16le over the
                          # websocket relay, lowest latency, but playback
                          # stops when a phone backgrounds/locks the page.
                          # aac: webui transcodes to AAC-LC for background /
                          # lock-screen playback (adds ~1-3 s latency).
  aac_bitrate_mono: 96000    # bits/sec, 32000-256000
  aac_bitrate_stereo: 128000 # bits/sec, 32000-256000

status:
  port: 8082              # localhost JSON status endpoint (0 = off)
  public:                 # what the unauthenticated GET / frame carries
    rds_text: true         # (default) now-playing + lock-screen text
    signal: false         # per-station RF level / SNR / diagnostics
    stereo: false         # stereo_frac -- the live STEREO badge (off -> badge hidden)
    pilot: false          # pilot_db / pilot_lock / pilot_hz (the detail behind it)
    rds_diag: false       # RDS PI / block errors / group rate
    sdr_health: false     # the `sdr` object (receiver-health banner)

scan:                     # passive band scan: GET /api/scan runs an averaged
                          # wideband periodogram of the SDR's current span and
                          # returns the PSD + detected carriers (the /admin
                          # page draws it and can add a station from a click).
                          # The FFT only runs while a scan request is pending.
  enabled: true            # false disables the endpoint entirely. Also
                          # accepted as a bare bool: `scan: false`.
  nfft: 4096               # FFT size, power of two, 256-65536.
                          # bin width = samp_rate / nfft.
  average_ms: 700          # accumulation window per scan, 100-15000.
  threshold_db: 8.0        # carrier-detection margin over the noise floor,
                          # in (0, 60].

decode:                   # listener-gated per-station decode. Off by
                          # default -- every configured station runs its DSP
                          # chain unconditionally, as it always has.
  listener_gated: false    # true: the real-time loop skips a station's chain
                          # while webui's relay reports no live listener for
                          # it (POST /active), reverting to decode-all if the
                          # relay goes quiet. Saves CPU on unwatched stations.
                          # Also accepted as a bare bool: `decode: true`.
```

**Defaults.** `agc`, `squelch`, `stereo`, `snr`, `rds`, `listener`, `status`,
`scan` and `decode` may all be omitted entirely. `agc` defaults to enabled
(`target 0.5`, `max_gain_db 40`, `response_ms 200`); `squelch` defaults to
disabled; `stereo` defaults to the values shown above; `snr` carries only
`k_noise` (calibrated SNR is always on); `rds` defaults to disabled;
`listener` defaults to `codec: pcm`; `status` defaults to off; `scan`
defaults to enabled with the values shown above; `decode` defaults to
`listener_gated: false` (every station always decodes). `agc` / `squelch` /
`rds` / `scan` / `decode` may also be given as a bare bool (`agc: false`)
instead of a mapping.

**Key derived rates** (all printed by `--check`):

| Quantity | Formula | Reference config |
|---|---|---|
| `bin_width` | `samp_rate / num_channels` | 320 kHz |
| `channel_rate` | `samp_rate × oversample / num_channels` | 640 kHz |
| usable per-bin BW | `bin_width × oversample` | 640 kHz |
| required per-bin BW | `2·(deviation + audio.bandwidth) + 2·predemod_transition` | 220 kHz |
| FM decimation | `channel_rate / intermediate_rate` (integer) | 10 |
| audio decimation | `intermediate_rate / audio.rate` (integer) | 2 |

### `stations.yml` reference

```yaml
stations:
- label: Radio One       # display name -- need not be unique
  freq: 94200000         # Hz
  port: 7355             # required, unique, 1025-65535 -- no auto-assignment
- label: Classic FM
  freq: 96400000
  port: 7356
  stereo: auto           # off (default) | true (always) | auto (blend on weak signal)
- label: Pop Wave
  freq: 99200000
  port: 7357
  agc: false             # per-station override of the global agc block
- label: Radio X
  freq: 103400000
  port: 7358
  squelch: {open_snr_db: 12, hang_ms: 500}   # per-station squelch override
- label: Golden Oldies
  freq: 104100000
  port: 7359
  rds: true              # per-station override of the global rds default
- label: Off Air FM
  freq: 105800000
  port: 7360
  enabled: false         # keep the entry (freq/port reserved), don't decode it
```

Per-station keys: `label` (required, display-only -- need not be unique),
`freq` (required, 50–1500 MHz, no `_` digit-grouping -- plain integer Hz),
`port` (**required**, unique, 1025–65535 -- every station must set its own,
there is no auto-assignment), `stereo` (optional), `agc` (optional; bool or
mapping), `squelch` (optional; bool or mapping), `rds` (optional; bool or
mapping), `enabled` (optional, default `true`). The `agc` / `squelch` / `rds`
overrides are merged onto the global block, so `squelch: {open_snr_db: 12}`
keeps the global `hang_ms`. `snr` is system-wide only (config.yml) -- the
calibrated path costs a few % across the whole pool, not per station.

The same station can appear more than once -- a second transmitter/frequency,
or a deliberate candidate set to compare -- as long as each entry has its own
`port`; `label` is display text only (webui keys everything off `port`), it's
never required to be unique.

`enabled: false` drops a station before bin/port planning -- it consumes no
channelizer bin, no worker, no UDP port, and is absent from `--check --json`
and `/api/stations/active`, but the label/freq/port stay in the file so it
can be switched back on later without re-typing them. Port uniqueness is
still enforced against disabled entries, so flipping one back on can't
silently collide with something added while it was off.

List **every station you own**. Ones outside the current receiver span are
skipped with a note rather than being an error, so the same `stations.yml`
works across different `center_freq` / `samp_rate` tunings.

An **empty list** — `stations: []`, or every entry `enabled: false` — is
allowed: the receiver comes up for the band scan (`GET /api/scan`) and the
admin endpoints only, with no per-station DSP. Build the list from the
`/admin` band scan, then Validate & Save. (A `stations:` key that's missing
entirely is still an error — that's a malformed file.)

### Validation: `--check`

`feedmyfm-rx --check` resolves the full plan and reports two classes of
problem.

**Hard errors** (receiver refuses to start, `--check` exits non-zero):

- `channelizer.num_channels` not a power of 2.
- `samp_rate` not divisible by `num_channels` (fractional bin width), or
  `samp_rate × oversample` not divisible by `num_channels` (fractional
  channel rate).
- `block_size` not a multiple of `num_channels / oversample`.
- `fm.intermediate_rate` not reachable from `channel_rate` by integer
  decimation (the message gives the nearest reachable value).
- `audio.rate` not an integer division of `fm.intermediate_rate`.
- `audio.bandwidth ≥ audio.rate / 2`, or `fm.intermediate_rate < 2 ×
  audio.bandwidth`.
- Usable per-bin bandwidth < required (Carson + 2·transition) — decrease
  `num_channels`, increase `oversample`, or raise `samp_rate`.
- A station's fine-mix offset exceeds half the bin width (lands on a bin
  edge) — retune `center_freq`.
- A station's FM half-bandwidth exceeds the channel Nyquist.
- A `stereo` station whose `channel_rate < 120 kHz`.
- Duplicate `port`; a `freq` outside 50–1500 MHz.
- No stations fall within the receiver span.
- An unknown key.

**Soft findings** (printed as `note:` lines / in `findings[]`, non-fatal):

- A station outside `center_freq ± samp_rate/2` — ignored.
- Usable bin bandwidth < 1.2× required — tight guard band, adjacent bleed
  possible.
- Two stations assigned the same bin — both decode from the same slice.
- A station within `75000 / oversample` Hz of its bin edge — possible
  adjacent interference or roll-off.

`--check --json` emits `{ok, audio_rate_hz, center_freq_hz, samp_rate_hz,
num_channels, bin_width_hz, channel_rate_hz, status_port, findings[],
stations[]}`, where each station has `label, freq_hz, port, stereo,
stereo_mode, bin, fine_hz, agc, squelch`.

### Retuning `center_freq`

`radio.center_freq` is a function of the station list, the sample rate, and
`num_channels` / `oversample` *together*. It's chosen to maximise the
worst-case distance of any station from a bin edge. If you change the station
list or any of those parameters, re-search it:

```bash
feedmyfm-rx -c deploy/config.yml -s deploy/stations.yml --optimize-cf --step-hz 500 --top 5
```

The grid search re-resolves the plan in-process for every candidate
(`--step-hz`, `--range-hz`, `--top`, `--allow-station-count-change`); no SDR
needed.

---

## Stereo decoding

Per station, `stereo:` is:

| Value | Behaviour | Wire format |
|---|---|---|
| `off` (default) | Mono. | 1-channel `s16le` |
| `true` | Always decode the 19 kHz pilot + 23–53 kHz L−R subcarrier. | 2-channel interleaved L/R `s16le` |
| `auto` | Decode, but crossfade L/R back toward mono when the pilot drops out or the composite SNR falls through the `stereo.blend_snr_*` window. | 2-channel interleaved L/R `s16le` |

`true` and `auto` **always** emit interleaved L/R frames, so the on-wire
format (and the web UI's `channels: 2`) never changes at runtime — `auto` just
makes L and R equal when it blends to mono. L−R noise is what kills
weak-signal stereo, which is what `auto` exists to manage.

**Decoder path** (`src/stereo_decoder.hpp`): composite low-pass → 19 kHz pilot
band-pass → regenerate the 38 kHz subcarrier with a narrow `nco_crcf` PLL
locked to the pilot, phase-doubled → coherent L−R DSB demod → L/R matrix with
a slewed blend factor → per-channel de-emphasis. Lock (`pilot_lock` on the
status port) needs both pilot power and PLL phase coherence; the tracked
pilot frequency is `pilot_hz`.

**Requirements & tuning**

- Needs `channel_rate ≥ 120 kHz` (the reference config gives 640 kHz).
  `--check` errors otherwise.
- Tune from the [status port](#status-port): `pilot_db` (19 kHz pilot power,
  dBc — should sit well above `stereo.pilot_threshold_db` when locked) and
  `stereo_frac` (0 = mono, 1 = full stereo).
- `make rx-stereo-selftest` runs the decoder end-to-end on a synthetic
  multiplex with no SDR and asserts pilot lock + > 25 dB L/R separation.
- Carrier polarity is a compiled constant derived from the filter
  design (`kStereoCarrierPhase` in `stereo_decoder.hpp`), verified by that
  selftest; there is no `FMRX_STEREO_INVERT` knob.

---

## AGC and squelch

Both run on the final audio, after de-emphasis / stereo matrixing, and both
are configured globally in `config.yml` with optional per-station overrides.

**Soft-mute, high-cut, AFC, a pilot notch, a look-ahead limiter and a
multipath indicator** run alongside them on the same per-station worker
path — pure DSP, no config knobs, so nothing to tune:

- **Soft-mute ramp.** The squelch cut is a ~12 ms / 25 ms raised gain
  envelope, not a hard `sample = 0`, so squelch edges and retunes don't
  click. The envelope also starts closed, so the first audio after a start
  or `--watch` reload fades in.
- **Progressive high-cut.** The final-audio LPF corner sweeps from the full
  audio bandwidth down toward ~4.5 kHz as `snr_db` falls (10 dB → floor,
  30 dB → wide open), slew-limited so it never whooshes. Buries weak-signal
  hiss the way a car radio's "HiCut" does. Live corner is `high_cut_hz` on
  the status port; equals the audio bandwidth whenever it's inactive.
- **AFC / carrier-drift tracking.** A slow integrator (frozen while
  squelched) folds the discriminator's DC mean back into the fine mixer,
  ±4 kHz, tracking TCXO drift and stations sitting off nominal. The
  correction is `afc_hz` on the status port — also a tuning-error readout.
- **19 kHz pilot notch.** A dedicated narrow notch on the final audio so
  the stereo pilot never reaches the encoder. Self-disabling at a 32 kHz
  `audio.rate` (19 kHz is above Nyquist there and the audio stages already
  bury it); it starts working once `audio.rate` is 44.1 / 48 kHz.
- **Look-ahead limiter.** A ~1.5 ms look-ahead brickwall limiter is the
  final stage before quantisation, reaching ~0 dBFS cleanly, avoiding the
  harmonic distortion a per-transient `tanh` soft-clip in the AGC would
  add — stereo limits L/R on one shared gain (no image shift).
- **Multipath indicator.** `multipath` on the status port is the normalised
  ripple of the pre-demod IF envelope as a 0…100 "%" figure — ~0 for a
  clean carrier, rising with frequency-selective multipath fading. Held
  while squelched. Indicator only; not wired into the blend yet.

`high_cut_hz`, `afc_hz` and `multipath` also surface on the listener page:
all three are always in the signal bar's hover title, and the readout line
under each station picks up a terse `· HiCut 6.2k`, `· multipath 24%` or
`· AFC 1.4k Hz` flag (with a plain-language tooltip) once the value crosses
an activity threshold — treble actually being cut, ≥ 12 % multipath, a
≥ 1 kHz carrier pull — alongside the existing `· muted` / `· AGC` flags.

**AGC** (`agc:`, on by default) tracks output RMS toward `target` with an
`response_ms` time constant, capped at `max_gain_db`, then applies that gain
linearly — peak control is the look-ahead limiter above, not a per-sample
knee. For a stereo station the gain is tracked off the L+R sum so both
channels move together (no image shift). `agc_gain_db` is reported on the
status port.

**Squelch** (`squelch:`, opt-in) mutes a station when an
ultrasonic-noise-ratio metric (discriminator power above 60 kHz vs total) says
there's no carrier. `open_snr_db` is the threshold to open; `hang_ms` holds it
open after the metric drops so speech pauses don't chop. `squelch_open` and
the driving metric — `squelch_metric_db` on the status port — tune the same
way: set `status.port`, watch `squelch_metric_db` on a live and a dead
channel, and pick a threshold between them.

> `squelch_metric_db` is a **squelch tuning proxy**, not an SNR — it's measured
> on the raw discriminator output and pumps with program loudness. Use it only
> for the squelch threshold. `snr_db` (below) is the calibrated figure.

**`snr_db`** is a calibrated, program-independent SNR in dB, comparable to
bench FM-tuner specs (~30 dB noisy … ~70 dB pristine carrier). It estimates
the FM discriminator's noise PSD from a clean out-of-band guard region — a
12 kHz window just below the pre-demod passband edge (76–88 kHz on the
75/15 kHz baseline), above RDS and the 67 kHz SCA — and integrates it through
de-emphasis over the audio band against a fixed reference-deviation sine
(AWGN-equivalent, mono, unweighted). One empirical scaling constant
(`kSnrKNoiseDefault` in `mono_station.hpp`) is fitted with `feedmyfm-rx
--snr-cal`; **re-run and re-bake it (or set `snr.k_noise`) after any change
to `channelizer.*` / `radio.samp_rate` / `fm.deviation` / `fm.tau` /
`audio.bandwidth` / `fm.predemod_transition`** — the SNR-driven features
(high-cut, stereo auto-blend, the webui weak-signal divider) all threshold
absolute dB against this figure. Blind spots: overmodulation / upstream
processing distortion don't register, and adjacent-channel splatter in the
guard band reads pessimistic — multipath has its own `multipath` readout
(above).

There is no proxy fallback: `snr_db` is always the calibrated figure. If
the channelizer plan can't fit the guard band (very low `channel_rate`, or
a narrow `fm.deviation`), `--check` fails rather than silently degrading
the metric. The `snr:` block now carries only `k_noise`; a legacy
`snr.calibrated` / `snr: false` is accepted but ignored with a `--check`
finding.

**Weak-signal demod** (`fm.demod` / `fm.declick`, both off by default).
`fm.declick: true` adds an impulsive-click
suppressor to the discriminator output (near the FM threshold the `atan2`
detector emits 2π phase-slip clicks); the suppression rate is `declick_ppm`
on the status port. `fm.demod: pll` swaps the open-loop discriminator for a
feedback PLL FM demodulator on **mono** stations, for ~1–2 dB of
threshold extension. Both *replace* per-sample work (CPU-neutral). Run
`--demod-cal` on your DSP baseline before enabling either — it prints the
true-SNR gain of each across the knee.

**Recalibrating `snr_db`.** The scaling constant `K_noise` is tied to the DSP
chain, not the signal, so re-fit it after changing any of: `channelizer.*`,
`radio.samp_rate`, `fm.deviation`, `fm.tau`, `audio.bandwidth` /
`audio.stop_bandwidth`, or `fm.predemod_transition`. (RF gain, station list,
AGC/squelch don't matter.) Run `make rx-snr-cal` (or `feedmyfm-rx -c … -s …
--snr-cal`): it sweeps carrier-to-noise ratio and prints `referenced dB` (the
live `snr_db`) against `true SNR dB`. If the last line says `FAIL`, copy the
printed `snr.k_noise: <value>` into `config.yml`'s `snr:` block — no rebuild,
`--watch` picks it up — or bake it into `kSnrKNoiseDefault` in
`mono_station.hpp` and rebuild. The ±2 dB check in that sweep is also wired
into CTest (`snr-calibration` in `rx/CMakeLists.txt`, `slow` label — runs
under `make rx-test-all`, not the default `make rx-test`) as a regression
gate.

---

## Status port

Set `status.port` (localhost only). `GET http://127.0.0.1:<port>/` returns:

```jsonc
{
  "ts": 1724965200,
  "sdr": {
    "link_up": true, "overruns": 0, "blocks": 84213, "rt_percent": 41.2,
    "gain_mode": "manual",     // config sdr.gain_mode
    "hardwaregain_db": 23.0,   // live AD9361 RX gain — the chip's own choice
                               //   in an AGC mode, the software loop's under auto_sw
    "peak_dbfs": -6.2,         // composite (whole-band) peak into the ADC, EMA
    "clip_ppm": 0.0            // full-scale samples per million (EMA) — non-zero
                               //   ⇒ front end clipping, intermods every station
  },
  "decode": {                  // config-mode fact, always present regardless
                               //   of status.public -- not a measurement
    "listener_gated": false,   // config decode.listener_gated
    "controlled": false        // true only once webui's relay has POSTed
                               //   /active recently; false = fail-open,
                               //   every station decodes regardless of the
                               //   toggle above
  },
  "stations": [
    {
      "label": "Radio One", "port": 7357,
      "idle": false,             // always present: true while listener-gated
                                 //   decode has this station's chain skipped
      "rf_dbfs": -18.4,          // channel power at the channelizer bin
      "snr_db": 58.0,            // calibrated AWGN-equivalent SNR, dB (see below)
      "squelch_metric_db": 21.0, // raw ultrasonic-noise-ratio proxy (squelch tuning)
      "squelch_open": true,
      "agc_gain_db": 6.0,
      "afc_hz": -430.0,         // carrier-drift correction folded into the fine
                                //   mixer (+ = station sits above nominal); 0 while
                                //   squelched
      "high_cut_hz": 15000,     // live audio-LPF corner: = audio bandwidth when the
                                //   signal is clean, falls toward ~4.5 kHz under noise
      "multipath": 2.4,         // IF-envelope ripple, 0..100 "%": ~0 clean, rises
                                //   with multipath fading; held while squelched
      "stereo": true,           // emits L/R frames
      "stereo_frac": 1.00,      // stereo stations only: 0 = mono … 1 = full stereo
      "pilot_db": -12.3,        // stereo stations only: 19 kHz pilot power, dBc
      "rds": {                  // present only for an `rds: true` station
                                //   currently being decoded; absent otherwise
        "lock": true,
        "pty": 10, "pty_name": "Pop Music",
        "tp": false, "ta": false,
        "ps": "RADIO ONE", "rt": "Now Playing: ...", "ptyn": "POP",
        "pi": 4660, "ct": "2026-09-13T12:00:00+02:00",
        "ber": 0.002, "groups_per_sec": 11.4, "groups_ok": 48213
      }
    }
  ]
}
```

`sdr.rt_percent` is the per-block CPU budget used (an EMA) — if it approaches
100 the box can't keep up. `overruns` counts dropped IIO buffers.
`sdr.hardwaregain_db` / `peak_dbfs` / `clip_ppm` are the front-end AGC
readouts — the listener page shows gain and, when it's non-zero, a clip warning next
to `RT %`. `webui` is host-networked and polls this directly (the relay
every 3 s), which is how the browser page shows live signal bars.

### `status.public` — what the unauthenticated frame carries

`GET /` is unauthenticated (`webui` proxies it straight to the public
listener page), so by default it now carries **only** the RDS programme
text (`rds_text`). The `sdr` object and every per-station metric above are
withheld unless the operator opts in — a fixed antenna's per-station RF
level / SNR / multipath / pilot frequency across the band, plus RDS PI and
clock time, together fingerprint the site. Turn groups back on under
`status.public` in `config.yml` (or on the **Server config** tab of
`/admin`):

```yaml
status:
  port: 8082
  public:
    signal: false      # per-station rf_dbfs/snr_db/squelch/agc/afc/high_cut/multipath/demod/declick
    stereo: false      # stereo_frac -- the live STEREO badge state (off -> the badge is hidden)
    pilot: false       # pilot_db / pilot_lock / pilot_hz -- the 19 kHz detail behind it
    rds_text: true     # rds.ps / rt / ptyn / pty / tp / ta  (now-playing + lock screen)
    rds_diag: false    # rds.pi / ber / groups_per_sec / groups_ok / ct
    sdr_health: false  # the whole `sdr` object (the receiver-health banner)
```

`GET /api/status/full` (HTTP Basic, same password as the write endpoints)
always returns the complete frame regardless — it backs the `/admin`
**Signal** tab. `--check --json` echoes the resolved policy under
`status_public`. `decode` is always present (it's a config-mode fact
`webui`'s relay needs, not a measurement).

---

## The web stack

Optional. One small FastAPI service (`webui`) plus an HTTPS front end. The
receiver does not depend on it.

> **Before you expose this past your own network:** streaming an off-air
> broadcast to other people is a retransmission, and that carries serious
> copyright, neighbouring-rights and broadcast-regulation exposure. See
> [Legal](#legal).

```
                       ┌──────────────────────────────────────────┐
  browser ──HTTPS──►   │  (your HTTPS   ──►  webui  (:8080)       │
                       │   front end)        │  UDP fan-out       │
                       │                     ▼                    │
                       │              feedmyfm-rx UDP audio       │
                       │                     ▲                    │
                       │  webui  ──HTTP──►  feedmyfm-rx  (:8082)  │
                       │  relay + /admin     │  status + config   │
                       │  proxy              ▼  API, in-process   │
                       │           config.yml / stations.yml (rw) │
                       └──────────────────────────────────────────┘
```

- **`feedmyfm-rx` HTTP port (`:8082`, config `status.host`/`status.port`)** —
  served by the receiver itself, no extra process. Endpoints:
  - `GET /` — the JSON status payload (SDR link, per-block health, per-station
    `rf_dbfs` / `snr_db` / squelch / agc). Unauthenticated, and trimmed to
    the `status.public` groups (see [Status port](#status-port)) — RDS
    programme text only by default.
  - `GET /api/status/full` — HTTP Basic auth. The complete status frame
    regardless of `status.public`; backs the `/admin` **Signal** tab.
  - `GET /api/stations/active` — unauthenticated. The resolved active plan:
    `{audio_rate, status_port, listener, stations:[{port,label,freq,stereo,stereo_mode}]}`.
    Returns `audio_rate: null` when the config currently doesn't resolve.
  - `GET/POST/PUT /api/config/raw`, `GET/POST/PUT /api/stations/raw` and
    `/validate` — HTTP Basic auth (`FEEDMYFM_RX_ADMIN_PASSWORD`, single shared
    password, username ignored; unset ⇒ these return 503). `PUT` validates
    in-process and rewrites the file in place only if it passes (422
    otherwise). A guessed password caps out at "write two files the receiver
    re-validates anyway".
  - Bind address is `status.host` (default `127.0.0.1`). A non-loopback
    `status.host` with no password set is refused at startup.
  - There is no TLS anywhere in this stack: `FEEDMYFM_RX_ADMIN_PASSWORD`
    crosses the network as plain HTTP Basic, both browser→webui→rx (webui's
    `/admin` proxy forwards the header unmodified) and, in the split-host
    topology below, webui→rx over the LAN. Treat a non-loopback
    `status.host` as reachable only from a trusted network / VPN, same as
    you would any other unencrypted admin surface — this password is not a
    real secret against anyone who can sniff that traffic.

- **`webui` (`:8080`)** — the listener page + relay. Holds **no
  config files**: it fetches the active plan from `RX_URL` every 12 s.
  - `GET /` — the listener page. `GET /api/stations/active`, `GET /api/status`
    (proxies the receiver status port). `WS /ws/stations/{port}/audio` — a
    one-time `stream_info` JSON frame (`sample_rate`, `channels`, `format`),
    then raw datagrams fanned out unmodified to every subscriber. No
    transcoding — lowest added latency.
  - `GET /stations/{port}/audio.aac` — used only when `config.yml`
    `listener.codec: aac`: webui transcodes the same PCM feed to AAC-LC
    (ADTS, ~32 ms frames) and streams it to a plain `<audio>` element wired
    to `navigator.mediaSession`, so phone **background / lock-screen
    playback** and next/prev station skip work. Lazy — an encoder exists
    only while a station has ≥ 1 AAC listener (~0.2–0.5 % of a webui-host
    core each). Costs ~1–3 s of added latency; the websocket endpoint stays
    live either way (`?pcm=1` forces it). `pcm` is the default and is
    byte-for-byte unchanged.
  - `GET /admin` + `/admin/api/{path}` — a transparent server-side proxy to
    `RX_URL`, passing the `Authorization` header through so webui never
    holds the password. The page's own login form owns the auth UI (a
    browser-native Basic-Auth dialog is unreliable across `fetch()`
    requests), so `WWW-Authenticate` is stripped on the way back rather
    than forwarded. The daemon's admin port therefore only needs to be
    reachable from webui, not from browsers.
  - Live signal bars require the webui to share a host with the receiver (it
    polls the receiver's loopback status port directly).
  - The AAC listener mode (`listener.codec: aac`, above) is a
    transcode-on-demand path that superseded an earlier Opus experiment;
    `pcm` remains the default.

### <a name="https-front-end"></a>HTTPS

The browser player uses `AudioWorklet` (in the default `pcm` mode), which the
browser only enables in a **secure context** — `https://` or `localhost`.
Plain `http://<lan-ip>:8080` never qualifies, so an HTTPS front end is
mandatory for any non-local listener. (`listener.codec: aac` needs no
`AudioWorklet` and works over plain `http://`, but a front end is still
required for `wss://` and the `?pcm=1` fallback.)

`webui` binds loopback-only (`127.0.0.1:8080`) — `deploy/compose.web.yml`
does not ship an HTTPS front end itself, so bring your own reverse proxy or
tunnel in front of it and point it at `http://127.0.0.1:8080`. Whatever you
use needs to pass the endless chunked AAC stream and the `wss://` relay
through without response-body buffering.

Config (`deploy/.env`, git-ignored — see `deploy/.env.example`):

```ini
FEEDMYFM_RX_ADMIN_PASSWORD=…    # gates the daemon's config write endpoints (blank ⇒ 503); .env.example ships ChangeMe -- set your own
```

### Deployment topologies

**All on one machine** (receiver + web stack):

```bash
docker compose -f deploy/compose.rx.yml up -d --build       # the receiver
docker compose -f deploy/compose.web.yml up -d --build     # webui
```

`deploy/compose.web.yml` runs webui loopback-only — put your own HTTPS
front end in front of it as the externally reachable path — see
[HTTPS](#https-front-end).

**Split: SDR host + separate listener host.** Run the receiver on the
SDR-facing host with a non-loopback `status.host` (e.g. `0.0.0.0`) and
`FEEDMYFM_RX_ADMIN_PASSWORD` set; run `webui` + the HTTPS front end on the
listener host with `RX_URL=http://<sdr-host>:8082`. Point `udp.host` in the
SDR host's `config.yml` at the listener host so the station audio arrives
there. Note the live signal bars still work in this split (webui polls
`RX_URL/` for them), but the split exposes the admin port on the LAN — hence
the mandatory password.

**Receiver only, no web stack.** Just `deploy/compose.rx.yml` (or the bare
binary). Consume the UDP ports with `ffplay` / VLC / your own code.

The two compose files are deliberately separate and
`deploy/compose.rx.yml` sets its own project name so
`docker compose -f deploy/compose.rx.yml down` doesn't tear down the other.

---

## Operations

- **`--watch`** (passed by `deploy/compose.rx.yml`): a `config.yml` /
  `stations.yml` edit — including one made through the webui admin page —
  re-execs the receiver in place. ~1–3 s audio gap while the SDR is
  re-acquired. An edit that doesn't validate is logged and ignored; the old
  plan keeps running.

- **SDR link loss** is handled in place: the reader reconnects the IIO context
  with backoff and streams silence downstream in the meantime, so a network
  blip to the SDR doesn't restart the process or drop listeners.
  `FMRX_SDR_FAULT_AFTER=N` injects one fake fault after N blocks to exercise
  the path.

- **Performance knobs** — all real tuning lives in `config.yml`. On a host
  that can't keep up (`rt_percent` near 100, `overruns` climbing):
  - `--threads N` — cap the worker pool.
  - lower `channelizer.proto_semilen_m` or `channelizer.atten_db` — cheaper
    channelizer, weaker adjacent rejection.
  - lower `radio.samp_rate` (fewer stations fit) or `channelizer.num_channels`.
  - `FMRX_TIMING=1` prints the per-block time budget (channelizer vs
    stations).

- **Environment variables**: `FMRX_TIMING`, `FMRX_DEBUG` (per-station level
  prints), `FMRX_SDR_FAULT_AFTER`, `FMRX_SNR_KNOISE`, `FMRX_FM_DEMOD` /
  `FMRX_FM_DECLICK` (override the resolved `fm.demod` / `fm.declick` for
  `--demod-cal` comparisons).

---

## Development

```bash
make build-rx            # build the receiver (-O3 -march=native)
make rx-test             # fast rx CTest suite (excludes slow DSP sweeps)
make rx-test-all         # full rx CTest suite, incl. the slow sweeps below (~100 s)
make rx-selftest         # synthetic two-tone FM through the real DSP; tone levels + SINAD
make rx-stereo-selftest  # synthetic FM stereo multiplex; pilot lock + L/R separation
make rx-sweep            # end-to-end audio frequency response vs ideal de-emphasis
make rx-snr-cal          # calibrated snr_db vs true recovered-audio SNR across C/N
make rx-demod-cal        # compare fm.demod / fm.declick options across the threshold knee
make rx-scan             # passive band-scan periodogram on a synthetic multiplex
make test                # webui's pytest suite: UDP→WS relay, AAC transcode, admin proxy, listener auth
make test-all            # same, without the `slow` marker filter
```

Filter taps are designed in-process with liquid's Kaiser designer, so output
is validated by **measurement** (`rx-selftest` / `rx-sweep`), not bit-matched
against a reference. `rx-selftest` asserts SINAD > 40 dB; `rx-sweep` asserts
the swept response tracks the ideal 50 µs de-emphasis curve; `rx-stereo-selftest`
asserts pilot lock and > 20 dB channel separation on a clean synthetic signal;
`rx-snr-cal` asserts the calibrated `snr_db` sits within ±2 dB of the true
recovered-audio SNR from 9–30 dB C/N.

**Where to add things.** All planning and validation belong in
`rx/src/resolve.hpp` — never in Python. Offline config tools are
`feedmyfm-rx` sub-commands (`--check`, `--scan`, `--optimize-cf`) that
reuse `resolve.hpp` directly; `webui` reads the resolved plan from the
daemon's HTTP port. New config knobs get a default, a bound check with a
key-naming error message, and a line in this README's
[`config.yml` reference](#configyml-reference).

Commit messages follow [Conventional Commits](https://conventionalcommits.org/).
See `CONTRIBUTING.md`.

---

## Troubleshooting

| Symptom | Likely cause / fix |
|---|---|
| `"No such device"` / IIO connect fails | Check the SDR is reachable at `sdr.ip` (`ping`, `iio_info -u ip:<addr>`). |
| `--check` exits non-zero | Read the message — it names the offending key and the constraint. See [Validation](#validation---check). |
| Station missing at startup with a `note:` | It's outside `center_freq ± samp_rate/2`, or two stations share a bin. Retune `radio.center_freq` (see [Retuning](#retuning-center_freq)). |
| No audio on a port | `udp.host` is `0.0.0.0` and you're listening from another machine — set it to that machine's address, or use the web relay. Also check the port with `--check`. |
| Audio dropouts / `overruns` climbing / `rt_percent` ≈ 100 | Host can't keep up: lower `samp_rate` / station count / `proto_semilen_m`, or pass `--threads N`. `FMRX_TIMING=1` shows where the time goes. |
| Weak / distorted audio | Adjust `sdr.gain_db` (measure ADC headroom — see the config reference above), check the antenna. |
| Stereo station sounds like swapped/hollow channels | Carrier polarity is a compiled constant (see [Stereo decoding](#stereo-decoding)), not a runtime knob. Confirm `pilot_db` is above `stereo.pilot_threshold_db` on the status port; if polarity itself is wrong, that's a `kStereoCarrierPhase` bug to fix in `stereo_decoder.hpp`, not a config issue. |
| `"address already in use"` on a station port | The web relay binds that port whenever a browser tab is on the station. Close the tab or stop `feedmyfm-webui`. |
| Browser player silent, console mentions secure context | The page must be served over HTTPS or from `localhost` (`AudioWorklet` requirement). See [HTTPS](#https-front-end). |
| `/admin` editor returns 503 | `FEEDMYFM_RX_ADMIN_PASSWORD` is unset on the receiver — the daemon's write endpoints are disabled. |
| `/admin` editor / station list unreachable | `RX_URL` doesn't point at a reachable `feedmyfm-rx` HTTP port (`status.host`/`status.port`). |
| receiver refuses to start, "status.host is … not loopback" | a non-loopback `status.host` requires `FEEDMYFM_RX_ADMIN_PASSWORD`. |
| Editing `deploy/config.yml` / `deploy/stations.yml` by hand doesn't reload the daemon (Docker) | `--watch` reads the file through a single-file bind mount, and an editor's atomic save (write + rename) swaps the inode the container is still holding. Save via `/admin` instead, or `docker compose -f deploy/compose.rx.yml up -d --force-recreate rx` after a manual edit. (Bare-metal `--watch`, and the daemon's own `/admin` writes, are unaffected — they don't rename.) |

---

## Further reading

- `CONTRIBUTING.md`.

---

## Legal

**Read this before you expose a stream to anyone but yourself.** The
GPL covers this *software*. It says nothing about the *content* you point
it at, and an off-air FM broadcast is some of the most heavily
rights-encumbered content there is.

Nothing here is legal advice, and the details are jurisdiction-specific.
But the shape of the problem is the same almost everywhere:

- **Receiving is fine; redistributing is the regulated act.** Private
  reception of a broadcast on your own antenna is what a radio is for.
  The moment you take that audio and make it reachable by other people —
  a public URL, a remote-access tunnel, a stream on the LAN that guests
  use — you are *retransmitting* / performing a "communication to the
  public", and that is a restricted act you normally need a licence for. Putting it on the internet reaches a "new public" and is treated
  as a fresh act of communication in its own right.
- **An FM signal is several rights stacked on top of each other**, and a
  retransmission licence has to clear all of them:
  - the **musical works** — composers and publishers, collected by your
    national performing-rights organisation;
  - the **sound recordings** — record labels and performers, collected by
    a neighbouring-rights society;
  - the **broadcast itself** — broadcasters hold a separate related right
    in their signal (Rome Convention / WIPO), so the station can object
    to a retransmission *regardless* of the music licensing.
- **Non-commercial and unaltered do not make it legal.** Not charging,
  not transcoding, "it's just a relay", low listener count — these affect
  how much you might owe, not whether the retransmission infringes.
- **Retransmission is often a licensed activity in its own right.**
  Several countries regulate anyone operating a retransmission service
  (registration with the national media regulator), on top of the
  copyright clearances.
- **The public internet is every jurisdiction at once.** A tunnelled
  stream is reachable worldwide, which can expose you to the rules — and
  the rightsholders and regulators — of every country a listener is in.
  Expect DMCA-style takedowns and host/CDN account termination (it
  violates the acceptable-use terms of most hosting and tunnel providers)
  as the *mild* outcome; statutory damages and, at commercial scale,
  criminal provisions exist in many places.

Uses that are normally defensible: listening to **your own** antenna feed
yourself, remotely, over a VPN or an authenticated single-user link
(reception, not "the public"); off-air **monitoring / logging**; stations
that have given you **written permission**; and genuinely licence-free
content. If you want a real public service, license it — talk to the
relevant PRO and neighbouring-rights society, and the broadcasters.

---

## License

GNU General Public License v3.0 or later — see [LICENSE](LICENSE). Copyleft:
use it, break it, improve it, but keep the source open.

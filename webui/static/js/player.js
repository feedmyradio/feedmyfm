// Main-thread audio player. Two listener modes, chosen once at startup
// from `<body data-listener-codec>` (set from config.yml `listener.codec`):
//
//   * pcm  -- opens a websocket per station on demand (lazily, on first
//             Play click), reads the server's stream_info frame to learn
//             the real sample rate/channel count (never assumed), and
//             feeds raw PCM to an AudioWorklet. Lowest latency; no
//             background playback on mobile.
//   * aac  -- one reused <audio> element fed the /stations/<port>/audio.aac
//             ADTS stream, wired to navigator.mediaSession so lock-screen
//             / background playback and next/prev station skip work.
//
// `?pcm=1` forces the pcm path regardless (debug / unsupported browser);
// the server keeps the websocket endpoint live in both modes.

const AAC_MODE =
  document.body.dataset.listenerCodec === "aac" &&
  !new URLSearchParams(location.search).has("pcm");

// Ordered station model -- the DOM list is already sorted by frequency.
const STATIONS = [...document.querySelectorAll(".station")].map((el) => ({
  port: Number(el.dataset.port),
  label: el.dataset.label,
  freqMhz: el.querySelector(".station-freq").textContent.trim(),
  artwork: el.dataset.artwork || "",
  el,
  button: el.querySelector(".play-btn"),
}));
let currentIndex = -1; // -1 = nothing playing

// Divider CSS floats just above the demoted weak-signal group. Created
// once here, appended to the list, and toggled by renderStats() so it
// only shows while at least one station is in that group.
const stationsList = document.querySelector("ul.stations");
const weakDivider = document.createElement("li");
weakDivider.className = "stations-weak-divider";
weakDivider.textContent = "Weak signal";
weakDivider.hidden = true;
if (stationsList) stationsList.appendChild(weakDivider);

// Live RDS for the *playing* station (pushed from the status socket), and
// the MediaMetadata object we handed the platform + which station index it
// was built for. Used to drive the lock-screen now-playing card (PS name
// as the artist line, RadioText as the title) without re-creating the
// metadata object on every RadioText change -- see applyMediaSession.
let currentRds = null;
let msMeta = null;
let msMetaIndex = -1;

function indexOfPort(port) {
  return STATIONS.findIndex((s) => s.port === port);
}

const players = new Map(); // port -> { ws, audioContext, workletNode, gainNode, button }

const VOLUME_STORAGE_KEY = "feedmyfm-volume";

function loadVolume() {
  const stored = parseFloat(localStorage.getItem(VOLUME_STORAGE_KEY));
  return Number.isFinite(stored) ? Math.min(1, Math.max(0, stored)) : 1;
}

// Only one station plays at a time (see stopAllExcept), so one global
// volume applies to whichever station is currently playing rather than
// tracking a separate level per station.
let currentVolume = loadVolume();

function wsUrlFor(port) {
  const scheme = location.protocol === "https:" ? "wss" : "ws";
  return `${scheme}://${location.host}/ws/stations/${port}/audio`;
}

// Tears down a station's websocket/audio AND resets its button -- must
// read button out of the entry before deleting it from the map, since
// that's the only place it's stored.
function stopStation(port) {
  const entry = players.get(port);
  if (!entry) return;
  players.delete(port);
  if (indexOfPort(port) === currentIndex) {
    currentIndex = -1;
  }
  entry.button.classList.remove("playing");
  if (entry.errored) {
    // Leave a red "Error" button behind until the user clicks it again
    // to retry.
    entry.button.textContent = "Error";
    entry.button.classList.add("error");
  } else {
    entry.button.textContent = "Play";
  }
  try {
    entry.ws.close();
  } catch (e) {
    /* ignore */
  }
  if (entry.audioContext) {
    try {
      entry.audioContext.close();
    } catch (e) {
      /* ignore */
    }
  }
}

function startStation(port, button) {
  const ws = new WebSocket(wsUrlFor(port));
  ws.binaryType = "arraybuffer";

  const entry = { ws, audioContext: null, workletNode: null, gainNode: null, button };
  players.set(port, entry);
  let channels = 1;

  ws.onmessage = async (event) => {
    if (typeof event.data === "string") {
      const info = JSON.parse(event.data);
      if (info.type === "stream_info") {
        channels = info.channels;
        const audioContext = new AudioContext({ sampleRate: info.sample_rate });
        await audioContext.audioWorklet.addModule("/static/js/audio-worklet-processor.js");
        const workletNode = new AudioWorkletNode(audioContext, "feedmyfm-processor", {
          outputChannelCount: [channels],
          processorOptions: { channels, sampleRate: info.sample_rate },
        });
        const gainNode = audioContext.createGain();
        gainNode.gain.value = currentVolume;
        workletNode.connect(gainNode);
        gainNode.connect(audioContext.destination);
        entry.audioContext = audioContext;
        entry.workletNode = workletNode;
        entry.gainNode = gainNode;
      }
      return;
    }

    if (!entry.workletNode) return;
    const samples = new Int16Array(event.data);
    let channelData;
    if (channels === 2) {
      const left = new Float32Array(samples.length / 2);
      const right = new Float32Array(samples.length / 2);
      for (let i = 0; i < left.length; i++) {
        left[i] = samples[i * 2] / 32768;
        right[i] = samples[i * 2 + 1] / 32768;
      }
      channelData = [left, right];
    } else {
      const mono = new Float32Array(samples.length);
      for (let i = 0; i < samples.length; i++) {
        mono[i] = samples[i] / 32768;
      }
      channelData = [mono];
    }
    entry.workletNode.port.postMessage(
      channelData,
      channelData.map((c) => c.buffer)
    );
  };

  ws.onclose = () => {
    stopStation(port);
  };
  ws.onerror = () => {
    entry.errored = true; // picked up by stopStation on the following onclose
  };
}

function stopAllExcept(exceptPort) {
  for (const port of Array.from(players.keys())) {
    if (port !== exceptPort) {
      stopStation(port);
    }
  }
}

// --- pcm mode: websocket + AudioWorklet, one station at a time ---------

function pcmPlayIndex(index) {
  const { port, button } = STATIONS[index];
  if (players.has(port)) return;
  stopAllExcept(port);
  button.textContent = "Stop";
  button.classList.add("playing");
  button.classList.remove("error"); // clear any stale error state on retry
  startStation(port, button);
  currentIndex = index;
}

// --- aac mode: one <audio> element + MediaSession ---------------------

const audioEl = document.getElementById("aac-audio");
let aacRepairTimer = null;
let aacBackoffMs = 1000;
let aacWaitingTimer = null;

function aacSrcFor(index) {
  return `/stations/${STATIONS[index].port}/audio.aac`;
}

// true from the tap until the first audio actually plays -- the media
// element pre-buffers a few seconds first (Safari especially), so the
// button shows a "Tuning…" pulse rather than jumping straight to "Stop".
let aacLoading = false;

function updateStationButtons() {
  STATIONS.forEach((s, i) => {
    const isCur = i === currentIndex;
    s.button.classList.toggle("playing", isCur && !aacLoading);
    s.button.classList.toggle("loading", isCur && aacLoading);
    s.button.classList.remove("error");
    // While loading the button text is empty -- the animated "..." is a
    // CSS ::after on .loading.
    s.button.textContent = isCur ? (aacLoading ? "" : "Stop") : "Play";
  });
}

const hasMediaSession = "mediaSession" in navigator && "MediaMetadata" in window;

function artworkList(url) {
  if (!url) return [];
  // Server pins artwork to PNG (see _resolve_artwork): always a
  // 512x512-ish raster, so one entry with an absolute URL is enough. The
  // URL must be absolute -- a relative or http: src is dropped on an
  // https: page.
  return [
    { src: new URL(url, location.href).href, sizes: "512x512", type: "image/png" },
  ];
}

// Lock-screen / media-key handlers. Registered at startup AND re-asserted
// on every applyMediaSession() -- iOS in particular only honours handlers
// once playback is genuinely active, and drops them on some transitions.
// Nulling the seek actions is what makes iOS show prev/next station in
// its two control slots instead of the default 15-second scrubber.
function registerMediaHandlers() {
  if (!hasMediaSession) return;
  const on = (action, handler) => {
    try {
      navigator.mediaSession.setActionHandler(action, handler);
    } catch (e) {
      /* action not supported on this platform -- ignore */
    }
  };
  on("play", () => audioEl && audioEl.play());
  on("pause", () => audioEl && audioEl.pause());
  on("stop", () => aacStop());
  on("previoustrack", () => stationStep(-1));
  on("nexttrack", () => stationStep(1));
  on("seekbackward", null);
  on("seekforward", null);
  on("seekto", null);
}
registerMediaHandlers();

// Lock-screen / control-centre text for a station + its current RDS.
// RadioText (the "grey" line -- programme / now-playing info) becomes the
// title; the RDS programme-service name (the "blue" line -- the station's
// own short name) becomes the artist. Both fall back to the station label
// / frequency when RDS isn't decoding or hasn't filled them in yet.
function mediaTexts(s, rds) {
  const ps = rds && rds.ps ? String(rds.ps).trim() : "";
  const rt = rds && rds.rt ? String(rds.rt).trim() : "";
  return {
    title: rt || s.label,
    artist: ps || (rt ? s.label : s.freqMhz),
  };
}

// Sync the existing MediaMetadata's text to the latest RDS *in place* --
// mutating title/artist doesn't disturb the platform's artwork fetch, so
// RadioText can update every few seconds without the logo flickering.
function updateMediaText() {
  if (!msMeta || msMetaIndex !== currentIndex) return;
  const t = mediaTexts(STATIONS[currentIndex], currentRds);
  try {
    if (msMeta.title !== t.title) msMeta.title = t.title;
    if (msMeta.artist !== t.artist) msMeta.artist = t.artist;
  } catch (e) {
    /* some engines expose MediaMetadata props as read-only -- live RDS
       just won't show there; the station name still does */
  }
}

// (Re)apply metadata + handlers + playbackState for the current station.
// Assigning a fresh MediaMetadata restarts the platform's artwork
// download, so once our session object is still installed for this station
// we keep it and only re-assert handlers/state + sync the RDS text --
// hammering the metadata (as the reassert ladder / media events do) would
// keep cancelling the artwork fetch before it lands ("title shows, logo
// doesn't").
function applyMediaSession(force) {
  if (!hasMediaSession || currentIndex < 0) return;
  const s = STATIONS[currentIndex];
  registerMediaHandlers();
  navigator.mediaSession.playbackState = "playing";
  const ours =
    msMeta &&
    msMetaIndex === currentIndex &&
    navigator.mediaSession.metadata === msMeta;
  if (!force && ours) {
    updateMediaText();
    return;
  }
  try {
    const t = mediaTexts(s, currentRds);
    msMeta = new MediaMetadata({
      title: t.title,
      artist: t.artist,
      album: "feedmyfm",
      artwork: artworkList(s.artwork),
    });
    msMetaIndex = currentIndex;
    navigator.mediaSession.metadata = msMeta;
  } catch (e) {
    /* MediaMetadata assignment failed -- ignore */
  }
}

let msReassertTimers = [];
function scheduleMediaSessionReasserts() {
  msReassertTimers.forEach(clearTimeout);
  // A couple of gentle nudges -- no-ops once the session matches (see
  // applyMediaSession), so they don't disturb the artwork fetch; they
  // only matter if the platform dropped the session.
  msReassertTimers = [600, 2500].map((d) => setTimeout(applyMediaSession, d));
}

function aacPlayIndex(index) {
  clearTimeout(aacRepairTimer);
  clearTimeout(aacWaitingTimer);
  aacBackoffMs = 1000;
  currentIndex = index;
  currentRds = null; // new station -- defaults until its RDS arrives
  audioEl.src = aacSrcFor(index);
  audioEl.volume = currentVolume;
  audioEl.play().catch(() => {
    /* autoplay rejection -- the click that got here is the user gesture,
       so this only fires on genuine failures, handled by the repair path */
  });
  aacLoading = true;
  updateStationButtons();
  applyMediaSession(true);
  scheduleMediaSessionReasserts();
}

function aacStop() {
  clearTimeout(aacRepairTimer);
  clearTimeout(aacWaitingTimer);
  msReassertTimers.forEach(clearTimeout);
  audioEl.pause();
  audioEl.removeAttribute("src");
  audioEl.load();
  currentIndex = -1;
  currentRds = null;
  msMeta = null;
  msMetaIndex = -1;
  aacLoading = false;
  if (hasMediaSession) {
    navigator.mediaSession.playbackState = "none";
    navigator.mediaSession.metadata = null;
  }
  updateStationButtons();
}

// The server stream is endless, so `ended` (server closed the response --
// station gone, encoder recycled) or `error` means the connection dropped:
// re-assign src after a capped backoff. `stalled` / `waiting` are NOT
// treated as fatal -- they fire routinely while the media element is still
// filling its start buffer, and reconnecting then just resets that buffer
// and thrashes (the "won't start until you reload" symptom). A long
// `waiting` with playback already under way gets one gentle nudge.
function scheduleAacRepair(delayMs) {
  if (currentIndex < 0 || !AAC_MODE) return;
  clearTimeout(aacRepairTimer);
  aacRepairTimer = setTimeout(() => {
    if (currentIndex < 0) return;
    audioEl.src = aacSrcFor(currentIndex);
    audioEl.play().catch(() => {});
    aacBackoffMs = Math.min(aacBackoffMs * 2, 5000);
  }, delayMs != null ? delayMs : aacBackoffMs);
}

if (audioEl) {
  audioEl.addEventListener("error", () => {
    scheduleAacRepair();
  });
  audioEl.addEventListener("ended", () => {
    scheduleAacRepair();
  });
  audioEl.addEventListener("waiting", () => {
    clearTimeout(aacWaitingTimer);
    // only nudge if we'd actually been playing (currentTime advanced)
    if (audioEl.currentTime > 0) {
      aacWaitingTimer = setTimeout(() => scheduleAacRepair(0), 8000);
    }
  });
  // Re-assert the media session once the element is actually playable /
  // playing -- iOS ignores a session populated before that point, which
  // is what left the lock screen blank.
  ["canplay", "playing"].forEach((ev) =>
    audioEl.addEventListener(ev, () => {
      clearTimeout(aacWaitingTimer);
      clearTimeout(aacRepairTimer);
      aacBackoffMs = 1000;
      applyMediaSession();
    })
  );
  // Real audio is flowing now -- drop the "Tuning…" state.
  audioEl.addEventListener("playing", () => {
    if (aacLoading) {
      aacLoading = false;
      updateStationButtons();
    }
  });
  audioEl.addEventListener("timeupdate", () => {
    if (aacLoading && audioEl.currentTime > 0) {
      aacLoading = false;
      updateStationButtons();
    }
  });
  // Cheap watchdog: if the session lost its metadata while we're playing
  // (some platforms clear it on a network blip), put it back -- but only
  // when it's actually missing, not on every frame.
  audioEl.addEventListener("timeupdate", () => {
    if (
      hasMediaSession &&
      currentIndex >= 0 &&
      (!navigator.mediaSession.metadata || msMetaIndex !== currentIndex)
    ) {
      applyMediaSession();
    }
  });
  audioEl.addEventListener("pause", () => {
    if (hasMediaSession && currentIndex >= 0) {
      navigator.mediaSession.playbackState = "paused";
    }
  });
}

// --- shared: play/stop dispatch + on-page prev/next ------------------

function playStation(index) {
  if (AAC_MODE) aacPlayIndex(index);
  else pcmPlayIndex(index);
}

// Station stepping is driven only by the lock-screen / media-key
// prev/next actions (registered above); there are no on-page buttons.
function stationStep(delta) {
  if (currentIndex < 0 || STATIONS.length < 2) return;
  playStation((currentIndex + delta + STATIONS.length) % STATIONS.length);
}

STATIONS.forEach((s, i) => {
  s.button.addEventListener("click", () => {
    if (i === currentIndex) {
      if (AAC_MODE) aacStop();
      else stopStation(s.port);
      return;
    }
    playStation(i);
  });
});

// --- receiver signal stats (feedmyfm-rx status port, proxied by the backend) ---

const rxHealthEl = document.getElementById("rx-health");

function fmtDb(v) {
  return (v >= 0 ? "+" : "") + v.toFixed(0);
}

// data.sdr.overruns is a lifetime counter that resets to 0 whenever the
// receiver re-execs (a config save under --watch, or a crash-restart), so the
// raw number is both ever-growing and not comparable across a reload. Keep a
// short trail of snapshots and report the rise over the last ~60 s instead.
const OVERRUN_WINDOW_MS = 60000;
const overrunTrail = []; // [{ t: epoch_ms, n: lifetime_count }, ...]

function overrunsLast60s(n) {
  const now = Date.now();
  if (overrunTrail.length && n < overrunTrail[overrunTrail.length - 1].n) {
    overrunTrail.length = 0; // counter went backwards -> receiver restarted
  }
  overrunTrail.push({ t: now, n });
  // Keep at most one sample older than the window, as the baseline to
  // subtract; drop anything older than that.
  while (
    overrunTrail.length > 2 &&
    now - overrunTrail[1].t >= OVERRUN_WINDOW_MS
  ) {
    overrunTrail.shift();
  }
  return n - overrunTrail[0].n;
}

// snr_db is always the calibrated, program-independent figure
// (AWGN-equivalent, mono, unweighted), ~= recovered
// audio SNR. Typical range: clean locals ~60-68, listenable-with-hiss
// mid-40s to upper-50s, fringe low-to-mid-30s.
function snrColor(snr) {
  if (snr >= 58) return "#22c55e";
  if (snr >= 45) return "#f59e0b";
  return "#ef4444";
}

// Live stereo badge: lit when the receiver is actually decoding stereo
// (stereo_frac > 0.5), dimmed otherwise -- for an `auto` station that
// means it has blended back to mono (weak signal / no pilot).
function renderStereoBadge(el, st) {
  const badge = el.querySelector(".badge-stereo");
  if (!badge) return;
  // The badge is a live-decode indicator (like the SNR figure). Without
  // stereo_frac in the frame -- rx `status.public.stereo` gated off, or no
  // status yet -- there's no live state to show, so hide it rather than
  // leave a dead badge that implies one.
  if (!st || typeof st.stereo_frac !== "number") {
    badge.hidden = true;
    return;
  }
  badge.hidden = false;
  if (st.idle) {
    // Listener-gated decode: nobody's tuned in, so the receiver isn't
    // running this station's stereo decoder -- don't imply a live state.
    badge.classList.add("is-mono");
    badge.title = "Idle — press Play to see live stereo status";
    return;
  }
  const on = st.stereo_frac > 0.5;
  badge.classList.toggle("is-mono", !on);
  let pilot = typeof st.pilot_db === "number" ? ` · pilot ${fmtDb(st.pilot_db)} dB` : "";
  // pilot_hz is the PLL-tracked 19 kHz pilot frequency; show it
  // when the loop reports a genuine phase lock.
  if (st.pilot_lock && typeof st.pilot_hz === "number")
    pilot += ` @ ${(st.pilot_hz / 1000).toFixed(2)} kHz`;
  // pilot_lock is a real PLL phase-lock flag -- distinguish "locked
  // but blended down on a weak L−R" from "never locked". Fall back to the
  // stereo_frac inference for an older rx that doesn't send pilot_lock.
  const locked = "pilot_lock" in st ? !!st.pilot_lock : on;
  badge.title = on
    ? `Stereo${pilot}`
    : locked
      ? `Auto stereo — pilot locked, blended to mono (weak L−R)${pilot}`
      : (badge.dataset.mode === "auto"
          ? `Auto stereo — no pilot lock${pilot}`
          : `Stereo — no pilot lock${pilot}`);
}

// RDS readout: the programme-service name (station's own short name) and
// its RadioText. Only present when rx is actually decoding RDS for this
// station -- i.e. it has `rds: true` in stations.yml AND someone is
// listening (listener-gated decode). The `rds` object is absent from the
// status frame otherwise, and we hide the line. Diagnostics (PI, PTY, TP/TA,
// group rate, block-error rate) go in the hover title to keep the row terse.
function renderStationRds(el, st) {
  const box = el.querySelector(".station-rds");
  if (!box) return;
  const r = st && !st.idle ? st.rds : null;
  // Once a station has ever reported RDS this session, keep its row
  // reserved -- a momentary drop (weak signal, a lost frame) shouldn't
  // collapse the row and shove the whole list up a line.
  if (r) el.classList.add("has-rds");
  if (!r) {
    // Momentary RDS drop on a station that has shown it this session --
    // keep the (empty) line so the row height doesn't change. CSS
    // min-height holds the space.
    if (st && !st.idle && el.classList.contains("has-rds")) {
      box.hidden = false;
      box.replaceChildren();
    } else {
      box.hidden = true;
      box.replaceChildren();
    }
    return;
  }

  const ps = (r.ps || "").trim();
  const rt = (r.rt || "").trim();
  const parts = [];
  if (ps) {
    const psEl = document.createElement("span");
    psEl.className = "rds-ps";
    psEl.textContent = ps;
    parts.push(psEl);
  }
  if (rt) {
    const rtEl = document.createElement("span");
    rtEl.className = "rds-rt";
    rtEl.textContent = rt;
    parts.push(rtEl);
  }
  // parts may be empty here: rx is decoding RDS but nothing human-readable
  // is through yet (pre-lock, or a locked carrier with empty PS/RT). Leave
  // the line blank -- CSS min-height keeps it reserved so it doesn't pop
  // in/out. The tooltip below still says "acquiring".

  const pi =
    typeof r.pi === "number"
      ? "PI " + r.pi.toString(16).toUpperCase().padStart(4, "0")
      : null;
  box.title = [
    r.lock ? "RDS locked" : "RDS acquiring",
    pi,
    r.pty ? r.pty_name : null,
    r.tp ? (r.ta ? "traffic announcement on air" : "carries traffic info") : null,
    r.ct ? "clock " + r.ct.slice(11, 16) + "Z" : null,
    typeof r.groups_per_sec === "number"
      ? `${r.groups_per_sec.toFixed(0)} groups/s`
      : null,
    typeof r.ber === "number"
      ? `${(r.ber * 100).toFixed(0)}% block errors`
      : null,
  ]
    .filter(Boolean)
    .join(" · ");

  box.hidden = false;
  box.replaceChildren(...parts);
}

function renderStationSignal(el, st) {
  renderStereoBadge(el, st);
  renderStationRds(el, st);
  const wrap = el.querySelector(".station-signal");
  if (!wrap) return;
  if (!st) {
    wrap.hidden = true;
    return;
  }
  wrap.hidden = false;
  const fill = wrap.querySelector(".signal-fill");
  const bar = wrap.querySelector(".signal-bar");
  const text = wrap.querySelector(".signal-text");

  // Listener-gated decode (rx decode.listener_gated): an `idle` station is
  // not being decoded, so every metric below is stale. Show that plainly
  // rather than a frozen bar/number that reads as current.
  if (st.idle) {
    wrap.classList.add("is-idle");
    if (fill) fill.style.width = "0%";
    if (bar) bar.title = "Not being decoded — no one is listening right now.";
    if (text) text.textContent = "idle";
    return;
  }
  wrap.classList.remove("is-idle");

  // The receiver withholds the per-station signal metrics unless the
  // operator has turned them on (rx config `status.public.signal`).
  // With them gone there's nothing to plot:
  // blank the bar + readout and stop before anything dereferences a
  // missing number. Keep `.station-signal` visible only if the STEREO
  // badge inside it is still showing -- renderStereoBadge() above has
  // already decided that (it hides the badge when status.public.stereo
  // is off too).
  const badge = wrap.querySelector(".badge-stereo");
  const badgeShown = badge && !badge.hidden;
  if (typeof st.rf_dbfs !== "number" && typeof st.snr_db !== "number") {
    if (fill) fill.style.width = "0%";
    if (bar) bar.hidden = true;
    if (text) text.textContent = "";
    wrap.hidden = !badgeShown;
    return;
  }
  if (bar) bar.hidden = false;

  // Bar length = received strength, placed in a shared self-scaling window
  // (see signalBarPct) so it ranks stations against each other rather than
  // claiming an absolute dBFS level -- which `rf_dbfs` can't give.
  const pct = signalBarPct(st.rf_dbfs);
  if (pct === null) {
    fill.style.width = "0%";
  } else {
    fill.style.width = pct.toFixed(0) + "%";
  }
  fill.style.background = snrColor(st.snr_db);
  if (bar) {
    // Live receiver-DSP diagnostics ride the bar's hover title always (the
    // ` · ` flags on the readout line only appear past an activity gate).
    const diag = [];
    if (typeof st.high_cut_hz === "number" && st.high_cut_hz > 0)
      diag.push(`high-cut ${(st.high_cut_hz / 1000).toFixed(1)} kHz`);
    if (typeof st.afc_hz === "number")
      diag.push(`AFC ${st.afc_hz >= 0 ? "+" : ""}${st.afc_hz.toFixed(0)} Hz`);
    if (typeof st.multipath === "number")
      diag.push(`multipath ${st.multipath.toFixed(0)}%`);
    if (st.pilot_lock && typeof st.pilot_hz === "number")
      diag.push(`pilot ${(st.pilot_hz / 1000).toFixed(2)} kHz`);
    if (st.demod === "pll") diag.push("PLL demod");
    if (typeof st.declick_ppm === "number" && st.declick_ppm > 0)
      diag.push(`declick ${st.declick_ppm.toFixed(0)} ppm`);
    bar.title =
      "Reception at a glance. Length = received signal strength ranked " +
      "against the other stations on this antenna (fuller is stronger); the " +
      "scale self-adjusts, so it's a ranking, not an absolute level. " +
      "Colour = how clean it sounds (the SNR number): green is clear, red " +
      "is poor." +
      (diag.length ? "\n\nReceiver: " + diag.join(" · ") : "");
  }

  // Per-metric tooltips carry the plain-language "what does this mean"
  // detail so the readout itself stays terse -- no "(proxy)" suffix on the
  // number, and no "· stereo/· mono" here (the STEREO badge shows that).
  // The dBFS / SNR pair travel together in the `signal` group, but build
  // the head from whichever numbers actually arrived so a trimmed frame
  // can't throw here.
  const head = [];
  if (typeof st.rf_dbfs === "number") {
    const dbfs = document.createElement("span");
    dbfs.title =
      "Raw RF level of this station's channel, in dB below the tuner's full " +
      "scale. It's a physical power reading with no fixed 'good' value -- a " +
      "strong local can sit well below 0 -- so use it to compare stations on " +
      "the same antenna, not as an absolute quality score.";
    dbfs.textContent = `${fmtDb(st.rf_dbfs)} dBFS`;
    head.push(dbfs);
  }
  if (typeof st.snr_db === "number") {
    const snr = document.createElement("span");
    snr.title =
      "How clean this station sounds, in dB. Higher is clearer: around 30 is " +
      "audibly hissy, 55 and up is essentially noise-free.";
    snr.textContent = `SNR ${st.snr_db.toFixed(0)} dB`;
    head.push(snr);
  }

  // Tail: a ` · `-joined series of receiver-state flags, each its own
  // <span> so the plain-language detail lives in a hover title and the
  // visible readout stays terse. Every flag has a deadband / activity gate
  // so a clean, strong, on-frequency station shows nothing past the SNR.
  // `.signal-text` is a fixed-height nowrap+ellipsis line, so a longer tail
  // never changes the row height (it just clips on a narrow screen).
  const tail = [];
  const addFlag = (label, title) => {
    const s = document.createElement("span");
    s.textContent = " · " + label;
    s.title = title;
    tail.push(s);
  };

  if (st.squelch_open === false)
    addFlag(
      "muted",
      "Squelched: the receiver can't find a carrier here and is muting the " +
        "station until one comes back."
    );

  // Multipath indicator: normalised IF-envelope ripple, 0..100%.
  // ~0 on a clean carrier; a 12% gate keeps it quiet until it's real.
  if (typeof st.multipath === "number" && st.multipath >= 12)
    addFlag(
      `multipath ${st.multipath.toFixed(0)}%`,
      "This station is arriving by several paths at once (reflections off " +
        "terrain or buildings), which muddies the sound and hurts stereo. " +
        "Higher is worse — re-aiming the antenna is what helps."
    );

  // high_cut_hz equals the audio bandwidth while the weak-signal treble
  // roll-off is inactive, so track the largest value seen for this port as
  // that ceiling and only flag once the corner is pulled well below it.
  if (typeof st.high_cut_hz === "number" && st.high_cut_hz > 0) {
    const ceil = Math.max(hiCutCeilByPort.get(st.port) || 0, st.high_cut_hz);
    hiCutCeilByPort.set(st.port, ceil);
    if (ceil > 0 && st.high_cut_hz < ceil * 0.92)
      addFlag(
        `HiCut ${(st.high_cut_hz / 1000).toFixed(1)}k`,
        `Weak-signal treble roll-off: audio above ~${(
          st.high_cut_hz / 1000
        ).toFixed(1)} kHz is being cut to bury hiss. It opens back up as ` +
          "reception improves."
      );
  }

  // Signed AGC make-up gain (negative = a hot station turned down, positive
  // = a quiet one boosted, capped by config max_gain_db). 2 dB deadband so
  // a station near unity doesn't flicker "±0".
  if (
    st.agc !== false &&
    typeof st.agc_gain_db === "number" &&
    Math.abs(st.agc_gain_db) >= 2
  )
    addFlag(
      `AGC ${fmtDb(st.agc_gain_db)}`,
      "Automatic level trim, in dB: negative means a loud station was turned " +
        "down, positive means a quiet one was boosted."
    );

  // AFC: carrier-drift correction folded into the tuner. A few
  // hundred Hz is routine TCXO / transmitter drift (and always in the bar's
  // hover title) -- only a ≥ 1 kHz pull, i.e. a genuinely off-nominal
  // station or big thermal drift, earns a flag on the readout line.
  if (typeof st.afc_hz === "number" && Math.abs(st.afc_hz) >= 1000)
    addFlag(
      `AFC ${(st.afc_hz / 1000).toFixed(1)}k Hz`,
      "The receiver is holding this station on frequency against more than a " +
        "kilohertz of drift (it's transmitting well off its nominal " +
        "frequency, or the tuner's oscillator has drifted with temperature)."
    );

  // Click suppressor (fm.declick): parts-per-million of discriminator
  // samples patched. ~0 on a clean signal; a 5 ppm gate keeps it quiet until
  // the station is actually into the FM threshold knee.
  if (typeof st.declick_ppm === "number" && st.declick_ppm >= 5)
    addFlag(
      `declick ${st.declick_ppm.toFixed(0)}ppm`,
      "This station is weak enough that the demodulator is producing " +
        "impulsive clicks, and the receiver is patching them out. Higher " +
        "means a noisier signal — it eases off as reception improves."
    );

  const headNodes = [];
  head.forEach((node, i) => {
    if (i) headNodes.push(document.createTextNode(" · "));
    headNodes.push(node);
  });
  text.replaceChildren(...headNodes, ...tail);
}

// Weak-signal demotion. A station whose reception is poor enough to be
// barely listenable gets a muted background and drops below the clear
// stations (CSS `order`, under the "Weak signal" divider) -- still
// playable, just out of the way so the list stays scannable.
//
// FM signals fade, so one status frame under the threshold isn't enough:
// we average snr_db over a rolling ~45 s window and only move a row once
// there are a few samples to go on, with drop/recover hysteresis so it
// doesn't ping-pong around the boundary.
//
// The thresholds are on the calibrated, program-independent snr_db scale
// (the only scale rx emits), ~= recovered audio
// SNR. Typical range: fringe stations that are genuinely hard work
// sit low-to-mid 30s (and their high-cut is engaged, blend is full mono);
// anything comfortably listenable is >~45. So demote below ~38, restore a
// few dB higher once it's clearly back. (Not a hard boundary -- nudge if a
// station that sounds fine gets demoted, or vice versa.) An `idle` station
// (listener-gated: nobody's decoding it, numbers are frozen) and the
// station you're currently playing are never demoted.
const WEAK_SNR_WINDOW_MS = 45000;
const WEAK_SNR_MIN_SAMPLES = 4;
const WEAK_SNR_DROP = 38; // mean calibrated snr_db below this -> demote
const WEAK_SNR_RECOVER = 43; // ...back at/above this -> restore

const snrTrails = new Map(); // port -> [{ t: epoch_ms, snr }]
const weakByPort = new Map(); // port -> bool, sticky between the two thresholds

// port -> largest high_cut_hz seen this session (≈ the station's audio
// bandwidth, which is what rx reports while the progressive high-cut is
// inactive). Used only to decide when the corner has dropped far enough to
// show a `· HiCut` flag; a monotonic max is fine here.
const hiCutCeilByPort = new Map();

// One shared low/high `rf_dbfs` window across every station, so the signal
// bars rank stations against each other ("fuller is stronger" down the
// list) instead of each self-scaling to 50%. `rf_dbfs` has no fixed usable
// range -- the front-end AGC pins the whole band's *composite* peak near
// -9 dBFS, so one narrow FM channel's RMS lands tens of dB lower even when
// it's a full-quieting local (the old fixed -60..0 map left every bar near
// empty). The ends widen instantly to admit a new extreme and relax back
// toward the midpoint with a ~4 min half-life, so an antenna swap or gain
// change re-levels over minutes and a lone deep fade heals on its own.
const RF_SPAN_MIN = 8; // never map a window narrower than this (avoids /~0)
const RF_SPAN_MAX = 50; // ...nor wider (one dropout shouldn't flatten it)
const RF_SPAN_HALFLIFE_MS = 240000;
let rfSpan = null; // { lo, hi, t } once the first reading lands

// Fold `dbfs` into the shared window and return its 0..100 place in it, or
// null when there's no usable reading yet.
function signalBarPct(dbfs) {
  if (typeof dbfs !== "number" || !Number.isFinite(dbfs)) return null;
  const now = Date.now();
  if (!rfSpan) {
    rfSpan = { lo: dbfs - RF_SPAN_MIN / 2, hi: dbfs + RF_SPAN_MIN / 2, t: now };
  } else {
    const mid = (rfSpan.lo + rfSpan.hi) / 2;
    const k = Math.pow(0.5, (now - rfSpan.t) / RF_SPAN_HALFLIFE_MS);
    rfSpan.lo = mid - (mid - rfSpan.lo) * k;
    rfSpan.hi = mid - (mid - rfSpan.hi) * k;
    rfSpan.t = now;
    if (dbfs < rfSpan.lo) rfSpan.lo = dbfs;
    if (dbfs > rfSpan.hi) rfSpan.hi = dbfs;
  }
  const span = rfSpan.hi - rfSpan.lo;
  if (span < RF_SPAN_MIN) {
    const c = (rfSpan.lo + rfSpan.hi) / 2;
    rfSpan.lo = c - RF_SPAN_MIN / 2;
    rfSpan.hi = c + RF_SPAN_MIN / 2;
  } else if (span > RF_SPAN_MAX) {
    rfSpan.lo = rfSpan.hi - RF_SPAN_MAX;
  }
  return Math.max(
    0,
    Math.min(100, ((dbfs - rfSpan.lo) / (rfSpan.hi - rfSpan.lo)) * 100)
  );
}

function updateWeakState(port, st) {
  const playing =
    currentIndex >= 0 &&
    STATIONS[currentIndex] &&
    STATIONS[currentIndex].port === port;
  // Nothing to judge by: no status, an idle (no-decode) row, or the
  // station in play. Forget its history, leave it in place.
  if (
    playing ||
    !st ||
    st.idle ||
    typeof st.snr_db !== "number"
  ) {
    snrTrails.delete(port);
    weakByPort.delete(port);
    return false;
  }
  const now = Date.now();
  const trail = snrTrails.get(port) || [];
  trail.push({ t: now, snr: st.snr_db });
  while (trail.length && now - trail[0].t > WEAK_SNR_WINDOW_MS) trail.shift();
  snrTrails.set(port, trail);
  if (trail.length < WEAK_SNR_MIN_SAMPLES) return weakByPort.get(port) || false;
  const mean = trail.reduce((a, s) => a + s.snr, 0) / trail.length;
  const was = weakByPort.get(port) || false;
  const weak = mean < WEAK_SNR_DROP ? true : mean >= WEAK_SNR_RECOVER ? false : was;
  weakByPort.set(port, weak);
  return weak;
}

function renderStats(data) {
  if (rxHealthEl) {
    if (data && data.sdr) {
      const up = data.sdr.link_up;
      const rt = Math.round(data.sdr.rt_percent);
      const recent = overrunsLast60s(data.sdr.overruns || 0);
      const sdr = data.sdr;

      // Front-end gain. hardwaregain_db is the
      // live AD9361 value -- the chip's own choice under an AGC mode, or
      // the software loop's under auto_sw. clip_ppm > 0 means the shared
      // ADC is saturating on the strongest carrier, which intermods every
      // station at once.
      let gainBit = "";
      if (typeof sdr.hardwaregain_db === "number") {
        const mode =
          sdr.gain_mode && sdr.gain_mode !== "manual" ? ` (${sdr.gain_mode})` : "";
        const peak =
          typeof sdr.peak_dbfs === "number"
            ? ` Composite peak ${sdr.peak_dbfs.toFixed(1)} dBFS.`
            : "";
        gainBit =
          ` · <span title="Front-end RF gain into the shared ADC.${peak}` +
          ` In an AGC mode the receiver sets this itself.">` +
          `gain ${sdr.hardwaregain_db.toFixed(1)} dB${mode}</span>`;
      }
      const clipBit =
        typeof sdr.clip_ppm === "number" && sdr.clip_ppm >= 1
          ? ` · <span class="rx-warn" title="Samples hitting the ADC full ` +
            `scale, per million. Non-zero means the front end is clipping on ` +
            `the strongest signal and distorting the whole band.">clip ` +
            `${sdr.clip_ppm.toFixed(0)} ppm</span>`
          : "";

      rxHealthEl.hidden = false;
      rxHealthEl.innerHTML =
        `<span class="dot${up ? "" : " down"}"></span>` +
        `SDR ${up ? "up" : "DOWN"}` +
        ` · <span title="How hard the receiver is working to keep every ` +
        `station playing in real time. Under 100% is healthy; sitting above ` +
        `100% means the audio will start to stutter.">RT ${rt}%</span>` +
        gainBit +
        clipBit +
        (recent
          ? ` · <span title="How many times in the last minute the receiver ` +
            `fell behind and dropped a bit of incoming signal. The odd blip ` +
            `is harmless; a steady climb means audio glitches.">` +
            `${recent} overruns/60s</span>`
          : "");
    } else {
      rxHealthEl.hidden = true;
    }
  }

  const byPort = new Map(
    (data && data.stations ? data.stations : []).map((s) => [s.port, s])
  );
  let anyWeak = false;
  document.querySelectorAll(".station").forEach((el) => {
    const port = Number(el.dataset.port);
    const st = byPort.get(port);
    renderStationSignal(el, st);
    const weak = updateWeakState(port, st);
    el.classList.toggle("is-weak", weak);
    anyWeak = anyWeak || weak;
  });
  weakDivider.hidden = !anyWeak;

  // Push the playing station's RDS to the lock-screen now-playing card.
  if (AAC_MODE && hasMediaSession && currentIndex >= 0) {
    const p = byPort.get(STATIONS[currentIndex].port);
    const rds = p && !p.idle && p.rds ? p.rds : null;
    if (JSON.stringify(rds) !== JSON.stringify(currentRds)) {
      currentRds = rds;
      updateMediaText();
    }
  }
}

// Pushed by the server (webui/relay.py's _stats_loop) rather than polled.
// Reconnects with a capped exponential backoff so a webui restart with the
// tab left open resumes on its own.
function connectStatusSocket() {
  let backoffMs = 1000;
  function open() {
    const scheme = location.protocol === "https:" ? "wss" : "ws";
    const ws = new WebSocket(`${scheme}://${location.host}/ws/status`);
    ws.onmessage = (event) => {
      backoffMs = 1000;
      try {
        const data = JSON.parse(event.data);
        // {"type": "codec_changed" | "stations_changed"}: the server-side
        // state this page was rendered from is stale (a codec flip would
        // otherwise strand it retrying a 409 on the wrong stream
        // endpoint forever; an admin add/remove otherwise never reaches
        // an already-open tab). A full reload is simplest and correct --
        // it's already what a manual refresh does.
        if (data.type === "codec_changed" || data.type === "stations_changed") {
          location.reload();
          return;
        }
        renderStats(data);
      } catch (e) {
        /* ignore a malformed frame, wait for the next one */
      }
    };
    // onerror is always followed by a close event for a connection
    // failure -- reconnecting from both would open duplicate sockets that
    // compound on every subsequent failure, so only onclose schedules one.
    ws.onclose = () => {
      setTimeout(open, backoffMs);
      backoffMs = Math.min(backoffMs * 2, 10000);
    };
  }
  open();
}

connectStatusSocket();

// The volume slider is a desktop control -- CSS hides it on touch
// devices (no software volume path on iOS media playback; hardware
// buttons elsewhere), so this just wires it for whoever still sees it.
const volumeSlider = document.getElementById("volume-slider");
if (volumeSlider) {
  volumeSlider.value = String(Math.round(currentVolume * 100));
  volumeSlider.addEventListener("input", () => {
    currentVolume = Number(volumeSlider.value) / 100;
    localStorage.setItem(VOLUME_STORAGE_KEY, String(currentVolume));
    if (AAC_MODE) {
      if (audioEl) audioEl.volume = currentVolume;
    } else {
      for (const entry of players.values()) {
        if (entry.gainNode) {
          entry.gainNode.gain.value = currentVolume;
        }
      }
    }
  });
}

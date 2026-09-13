// Admin editor for config.yml/stations.yml, talking to the feedmyfm-rx
// through this server's /admin/api/* proxy (see routes/admin.py).
//
// config.yml is still a raw YAML textarea (it's heavily commented,
// one-time setup). stations.yml is a structured row editor: the file is
// parsed on load (js-yaml), edited as fields, and re-emitted as plain
// YAML on Validate/Save through the same /raw endpoints. Round-tripping
// drops any comments and normalises formatting -- stations.yml carries
// no comments worth keeping, and the rx daemon still validates the
// re-emitted text before it's written.
//
// Auth is a single shared password (rx admin_routes.hpp), sent as HTTP
// Basic on every request. The browser's own native Basic-Auth dialog is
// deliberately not used for this -- fetch()-initiated 401s don't
// reliably trigger it (that assumption was tried and was wrong: it's
// inconsistent across browsers), so this page owns a real login form
// instead and handles 401s itself; the proxy also strips
// WWW-Authenticate so the native dialog can't pop up alongside it.
//
// The password lives in sessionStorage rather than being re-typed on
// every reload -- it survives a reload but clears when the tab closes,
// which doubles as "log out" without needing any server-side session.

const SESSION_KEY = "feedmyfm-admin-password";
let adminPassword = sessionStorage.getItem(SESSION_KEY);

const loginForm = document.getElementById("login-form");
const passwordInput = document.getElementById("admin-password");
const authStatusEl = document.querySelector(".auth-gate .auth-status");
const authGateEl = document.querySelector(".auth-gate");
const contentEl = document.getElementById("admin-content");
const logoutBtn = document.getElementById("logout-btn");

function authHeader() {
  // Username is unchecked/ignored server-side -- this is a single
  // shared password, not per-user accounts (see rx admin_routes.hpp).
  return "Basic " + btoa(":" + adminPassword);
}

// Central 401 handling: any request built on authFetch() that comes back
// unauthorized drops straight back to the login form, clearing the stored
// password so a stale/wrong one can't keep silently failing.
function showLoggedOut(message) {
  adminPassword = null;
  sessionStorage.removeItem(SESSION_KEY);
  stopSignalPolling();
  contentEl.hidden = true;
  authGateEl.hidden = false;
  authStatusEl.textContent = message || "";
  passwordInput.value = "";
  passwordInput.focus();
}

function showLoggedIn() {
  authGateEl.hidden = true;
  contentEl.hidden = false;
}

async function authFetch(path, options = {}) {
  const response = await fetch(path, {
    ...options,
    headers: { ...(options.headers || {}), Authorization: authHeader() },
  });
  if (response.status === 401) {
    showLoggedOut("Incorrect password.");
    throw new Error("Incorrect password.");
  }
  return response;
}

function renderResult(el, result) {
  el.innerHTML = "";
  if (result.ok) {
    const ok = document.createElement("p");
    ok.className = "result-ok";
    ok.textContent = "Valid.";
    el.appendChild(ok);
    for (const w of result.warnings || []) {
      const p = document.createElement("p");
      p.className = "result-warning";
      p.textContent = w;
      el.appendChild(p);
    }
    return;
  }

  for (const err of result.field_errors || []) {
    const p = document.createElement("p");
    p.className = "result-error";
    p.textContent = `${err.loc.join(".")}: ${err.msg}`;
    el.appendChild(p);
  }
  for (const msg of result.general_errors || []) {
    const pre = document.createElement("pre");
    pre.className = "result-error";
    pre.textContent = msg;
    el.appendChild(pre);
  }
  if (!(result.field_errors || []).length && !(result.general_errors || []).length) {
    const p = document.createElement("p");
    p.className = "result-error";
    p.textContent = "Rejected (no details returned).";
    el.appendChild(p);
  }
}

async function apiCall(kind, action, method, body) {
  const response = await authFetch(`/admin/api/${kind}/${action}`, {
    method,
    headers: { "Content-Type": "application/json" },
    body: body !== undefined ? JSON.stringify(body) : undefined,
  });
  if (response.status === 503) {
    throw new Error("No rx daemon URL configured for this webui instance (--rx-url).");
  }
  const data = await response.json();
  // PUT rejections come back as {"detail": {...the same shape...}}
  return data.detail !== undefined ? data.detail : data;
}

// --- config.yml: raw textarea editor (unchanged) -------------------

// The curated field set. Bounds/defaults mirror rx/src/resolve.hpp. Blocks
// not listed here (sdr.ip, udp.*, status.*) are never shown but ride
// through parse -> dump untouched. `k`: config key; `t`: control type
// (int | float | mhz | bool | enum). agc/squelch/stereo/snr/scan/rds are
// scalar-or-map in the file -- normalizeConfigModel() coerces the scalar
// form to a map so the fields have somewhere to write.
const CONFIG_SCHEMA = [
  { block: "sdr", label: "SDR", open: true, fields: [
    { k: "gain_db", label: "Gain (dB)", t: "int", min: 0, max: 73, def: 21,
      help: "Pluto RX gain. In manual it's fixed here; in an AGC mode it's the startup value (and the ceiling under auto_sw). Re-measure via the band scan before raising -- too high clips the ADC on the hottest carrier." },
    { k: "gain_mode", label: "Gain mode", t: "enum", def: "manual",
      options: [["manual", "manual"], ["slow_attack", "slow_attack"], ["fast_attack", "fast_attack"], ["hybrid", "hybrid"], ["auto_sw", "auto_sw"]],
      help: "manual = fixed gain. slow/fast/hybrid = the AD9361's own AGC on the whole-band composite (hybrid = clip-protection only). auto_sw = the software peak-headroom loop below. Takes effect on receiver restart." },
  ]},
  { block: "sdr.agc", label: "Front-end AGC loop (auto_sw only)", fields: [
    { k: "target_dbfs", label: "Composite peak target (dBFS)", t: "float", def: -9,
      help: "The software loop nudges the front-end gain to hold the whole-band peak here. Only read when gain mode is auto_sw." },
    { k: "max_dbfs", label: "Step-down above (dBFS)", t: "float", def: -3,
      help: "Peak above this drops the gain immediately, skipping the interval gate." },
    { k: "min_gain_db", label: "Min gain (dB)", t: "float", def: 0,
      help: "Lower clamp. The upper clamp is sdr.gain_db." },
    { k: "step_db", label: "Step size (dB)", t: "float", def: 1 },
    { k: "interval_ms", label: "Min step interval (ms)", t: "int", def: 2000 },
    { k: "hysteresis_db", label: "Step-up hysteresis (dB)", t: "float", def: 3,
      help: "Gain only rises when the peak is this far below target." },
    { k: "clip_ppm_trip", label: "Clip trip (ppm)", t: "float", def: 5,
      help: "Clipped-sample rate that forces an immediate step down." },
  ]},
  { block: "radio", label: "Radio", open: true, fields: [
    { k: "center_freq", label: "Center freq (MHz)", t: "mhz", def: 98000000,
      help: "SDR tune frequency. A station must fall within +/- samp_rate/2 of this to be decoded." },
    { k: "samp_rate", label: "Sample rate (Hz)", t: "int", def: 20480000,
      help: "Total captured RF span. Must divide evenly by channelizer.num_channels." },
  ]},
  { block: "channelizer", label: "Channelizer", fields: [
    { k: "num_channels", label: "Channels", t: "int", def: 64, help: "Power of two." },
    { k: "oversample", label: "Oversample", t: "int", min: 1, def: 2, help: "Must evenly divide num_channels." },
    { k: "atten_db", label: "Prototype atten (dB)", t: "int", def: 60 },
    { k: "proto_semilen_m", label: "Prototype semi-length", t: "int", def: 12 },
    { k: "block_size", label: "Block size", t: "int", def: 131072, help: "Multiple of num_channels/oversample." },
  ]},
  { block: "fm", label: "FM demod", fields: [
    { k: "deviation", label: "Deviation (Hz)", t: "int", def: 75000 },
    { k: "tau", label: "De-emphasis tau (s)", t: "float", def: 5e-5, help: "50e-6 in region 1 (EU), 75e-6 in the Americas / Korea." },
    { k: "intermediate_rate", label: "Intermediate rate (Hz)", t: "int", def: 64000 },
    { k: "predemod_transition", label: "Predemod transition (Hz)", t: "int", def: 20000 },
    { k: "demod", label: "Demodulator", t: "enum", def: "discriminator",
      options: [["discriminator", "discriminator"], ["pll", "pll"]],
      help: "discriminator = the open-loop atan2 detector. pll = a feedback FM demodulator for ~1-2 dB of weak-signal threshold extension -- MONO stations only (forced back to discriminator for stereo/RDS). Run --demod-cal on this DSP baseline first. Takes effect on receiver restart." },
    { k: "declick", label: "Click suppressor", t: "bool", def: false,
      help: "Removes the impulsive 2-pi phase-slip clicks the discriminator emits near the FM threshold. Small clean gain in the knee, neutral above it. Reported as declick_ppm on the status port." },
    { k: "declick_sigma", label: "Click threshold (sigma)", t: "float", def: 5,
      help: "Robust-deviation multiplier that marks a sample as a click. Lower = more aggressive; below ~4.5 it starts firing on noise peaks. Only read when the click suppressor is on." },
    { k: "pll_bw_hz", label: "PLL demod loop BW (Hz)", t: "int", def: 0,
      help: "0 = compiled default (45 kHz). Only read for demod: pll." },
    { k: "afc", label: "AFC (carrier-drift tracking)", t: "bool", def: true,
      help: "On: a slow loop folds the discriminator DC mean into the tuner to hold each station on frequency. Turn OFF (static mixer) for a signal where heavy multipath corrupts that estimate and the loop warbles the audio. Takes effect on receiver restart." },
    { k: "limiter", label: "Look-ahead limiter", t: "bool", def: true,
      help: "On: a short look-ahead brickwall limiter is the peak stage before quantisation. OFF: memoryless tanh soft-clip inside the AGC instead. Takes effect on receiver restart." },
  ]},
  { block: "audio", label: "Audio", fields: [
    { k: "rate", label: "Rate (Hz)", t: "int", def: 32000 },
    { k: "bandwidth", label: "Bandwidth (Hz)", t: "int", def: 15000, help: "Must be < rate/2." },
    { k: "stop_bandwidth", label: "Stop bandwidth (Hz)", t: "int", def: 19000 },
  ]},
  { block: "agc", label: "AGC (fleet default)", scalarKey: "enabled", fields: [
    { k: "enabled", label: "Enabled", t: "bool", def: true },
    { k: "target", label: "Output RMS target", t: "float", def: 0.5, help: "0..1. Lower keeps peaks under the soft-clip knee; ~0.2-0.3 is safe." },
    { k: "max_gain_db", label: "Max makeup gain (dB)", t: "float", def: 40, help: "Cap on upward gain. Low (~6) keeps quiet passages quiet." },
    { k: "response_ms", label: "Response (ms)", t: "float", def: 200 },
  ]},
  { block: "squelch", label: "Squelch (fleet default)", scalarKey: "enabled", fields: [
    { k: "enabled", label: "Enabled", t: "bool", def: false },
    { k: "open_snr_db", label: "Open threshold (dB)", t: "float", def: 20 },
    { k: "hang_ms", label: "Hang (ms)", t: "float", def: 800 },
  ]},
  { block: "stereo", label: "Stereo (fleet default)", scalarKey: "mode", fields: [
    { k: "mode", label: "Mode", t: "enum", options: [["mono", "mono"], ["stereo", "stereo"], ["auto", "auto"]], def: "mono" },
    { k: "pilot_threshold_db", label: "Pilot lock threshold (dBc)", t: "float", def: -30 },
    { k: "blend_snr_lo_db", label: "Auto blend: full mono ≤ SNR (dB)", t: "float", def: 20 },
    { k: "blend_snr_hi_db", label: "Auto blend: full stereo ≥ SNR (dB)", t: "float", def: 34, help: "Calibrated-SNR scale (not the squelch-metric proxy) -- runs ~10-15 dB higher. Blend also caps on multipath % automatically." },
    { k: "pilot_pll", label: "Pilot PLL", t: "bool", def: true,
      help: "On: a narrow PLL locks the 19 kHz pilot and phase-doubles it for the 38 kHz subcarrier. OFF: normalise-and-square regen instead. Takes effect on receiver restart." },
  ]},
  { block: "snr", label: "SNR readout", fields: [
    { k: "k_noise", label: "k_noise override", t: "float", def: 0, help: "0 = use the value fitted into the binary. Set from --snr-cal after a DSP-chain change. (Calibrated SNR is always on; there is no proxy fallback.)" },
  ]},
  { block: "listener", label: "Listener stream", fields: [
    { k: "codec", label: "Codec", t: "enum", options: [["pcm", "pcm"], ["aac", "aac"]], def: "pcm" },
    { k: "aac_bitrate_mono", label: "AAC bitrate, mono", t: "int", def: 96000 },
    { k: "aac_bitrate_stereo", label: "AAC bitrate, stereo", t: "int", def: 128000 },
  ]},
  { block: "scan", label: "Band scan", scalarKey: "enabled", open: true, fields: [
    { k: "enabled", label: "Enabled", t: "bool", def: true },
    { k: "average_ms", label: "Window (ms)", t: "int", min: 100, max: 15000, def: 700, help: "Longer digs weaker carriers out of the noise; > ~5000 can abort client-side." },
    { k: "nfft", label: "FFT size", t: "int", def: 4096, help: "Power of two, 256-65536." },
    { k: "threshold_db", label: "Detection margin (dB)", t: "float", def: 8 },
  ]},
  { block: "decode", label: "Decode", scalarKey: "listener_gated", fields: [
    { k: "listener_gated", label: "Listener-gated", t: "bool", def: false, help: "Only decode a station while webui reports a live listener for it." },
  ]},
  { block: "rds", label: "RDS (fleet default)", scalarKey: "enabled", fields: [
    { k: "enabled", label: "Enabled", t: "bool", def: false, help: "Per-station rds: true|false in stations.yml overrides this." },
  ]},
  // status.public.* -- which groups of the status JSON the public listener
  // page (webui's unauthenticated /api/status proxy) is allowed to see.
  // Off = the receiver withholds those fields
  // from /api/status entirely; the /admin Signal tab always sees them.
  // status.port / status.host have no field and ride through untouched.
  { block: "status.public", label: "Listener-page status visibility", fields: [
    { k: "signal", label: "Signal metrics (RF level, SNR, multipath…)", t: "bool", def: false,
      help: "The per-station signal bar and SNR/diagnostics line on the public listener page. Off withholds rf_dbfs / snr_db / squelch / agc / afc / high_cut / multipath / demod / declick from /api/status entirely -- a fixed antenna's per-station levels across the band are a site fingerprint." },
    { k: "stereo", label: "Live stereo state", t: "bool", def: false,
      help: "Whether each station is decoding in stereo right now (stereo_frac) -- lights the STEREO badge on the listener page and shows the auto-blend state. Off: no stereo_frac in the frame, and the listener page drops the STEREO badge rather than show a dead one." },
    { k: "pilot", label: "Pilot-tone detail", t: "bool", def: false,
      help: "The 19 kHz pilot diagnostics behind the stereo state: pilot_db (power, dBc), pilot_lock (PLL phase lock), pilot_hz (tracked frequency). Independent of the live stereo state above." },
    { k: "rds_text", label: "RDS programme text (PS / RadioText)", t: "bool", def: true,
      help: "The now-playing line and the phone lock-screen text (rds.ps / rt / ptyn / pty / tp / ta). Off removes RDS text from the listener page." },
    { k: "rds_diag", label: "RDS diagnostics (PI, block errors, group rate)", t: "bool", def: false,
      help: "The station identity code and decoder-health numbers (rds.pi / ber / groups_per_sec / groups_ok / ct)." },
    { k: "sdr_health", label: "Receiver health banner", t: "bool", def: false,
      help: "The link / real-time-load / overrun / gain / clip strip at the top of the listener page (the whole `sdr` object)." },
  ]},
];
const CONFIG_SCALAR_BLOCKS = Object.fromEntries(
  CONFIG_SCHEMA.filter((b) => b.scalarKey).map((b) => [b.block, b.scalarKey]),
);

const configSection = document.getElementById("config-editor");
const configBlocksEl = configSection.querySelector(".config-blocks");
const configTextarea = configSection.querySelector(".editor-text");
const configValidateBtn = configSection.querySelector(".validate-btn");
const configSaveBtn = configSection.querySelector(".save-btn");
const configCancelBtn = configSection.querySelector(".cancel-btn");
const configStatusEl = configSection.querySelector(".editor-status");
const configResultEl = configSection.querySelector(".editor-result");
const configCountEl = configSection.querySelector(".config-count");

let configModel = {}; // parsed config.yml, the source of truth
let configDirty = false;
let configSavedYaml = "";

function markConfigDirty() {
  configDirty = true;
  configCountEl.textContent = "unsaved changes";
  configCancelBtn.disabled = false;
}

function markConfigClean() {
  configDirty = false;
  configCountEl.textContent = "";
  configCancelBtn.disabled = true;
}

// scalar form (`rds: true`, `stereo: auto`) -> map form so a field has a
// home; an absent block stays absent.
function normalizeConfigModel(cfg) {
  for (const [block, scalarKey] of Object.entries(CONFIG_SCALAR_BLOCKS)) {
    const v = cfg[block];
    if (v !== undefined && (typeof v !== "object" || v === null || Array.isArray(v))) {
      cfg[block] = { [scalarKey]: v };
    }
  }
  // `status:` also accepts a bare port scalar (`status: 8082`). The
  // status.public.* fields need a map to write into, so coerce it the way
  // rx's resolver reads it -- otherwise editing a visibility toggle would
  // clobber the port.
  if (
    cfg.status !== undefined &&
    (typeof cfg.status !== "object" || cfg.status === null || Array.isArray(cfg.status))
  ) {
    cfg.status = { port: cfg.status };
  }
  return cfg;
}

// A schema `block` may be a dotted path ("sdr.agc") for a nested map. Walk
// it; `create` builds missing intermediate maps, else returns undefined.
function configBlockObj(model, blockPath, create) {
  const parts = blockPath.split(".");
  let o = model;
  for (const p of parts) {
    if (o[p] === undefined || o[p] === null || typeof o[p] !== "object") {
      if (!create) return undefined;
      o[p] = {};
    }
    o = o[p];
  }
  return o;
}

// After removing a key from a nested block, drop now-empty maps up the
// chain (but never the top-level block) so we don't emit `agc: {}`, which
// rx rejects unless gain_mode is auto_sw.
function pruneEmptyBlock(model, blockPath) {
  const parts = blockPath.split(".");
  for (let depth = parts.length; depth >= 2; depth--) {
    const parent = configBlockObj(model, parts.slice(0, depth - 1).join("."), false);
    const key = parts[depth - 1];
    if (parent && parent[key] && typeof parent[key] === "object" &&
        Object.keys(parent[key]).length === 0) {
      delete parent[key];
    }
  }
}

function loadConfigFromYaml(yamlText) {
  configSavedYaml = yamlText;
  try {
    configModel = normalizeConfigModel(jsyaml.load(yamlText) || {});
  } catch (e) {
    configStatusEl.textContent = `could not parse config.yml: ${e.message}`;
    configModel = {};
  }
  configTextarea.value = emitConfigYaml(configModel);
  renderConfigBlocks();
  markConfigClean();
}

function emitConfigYaml(model) {
  return jsyaml.dump(model, { sortKeys: false, lineWidth: -1, indent: 2 });
}

function coerceConfigValue(field, raw) {
  if (field.t === "bool") return !!raw;
  if (field.t === "enum") return raw;
  if (raw === "" || raw == null) return undefined;
  if (field.t === "mhz") {
    const mhz = parseFloat(raw);
    return Number.isFinite(mhz) ? Math.round(mhz * 1e6) : undefined;
  }
  const n = field.t === "int" ? parseInt(raw, 10) : parseFloat(raw);
  return Number.isFinite(n) ? n : undefined;
}

function fieldDisplayValue(field, val) {
  if (val === undefined) return "";
  if (field.t === "mhz") return String(val / 1e6);
  return String(val);
}

// stereo `mode` is bool|"auto" in the file; the select uses mono/stereo/auto.
function readStereoMode(v) {
  if (v === "auto" || v === "Auto" || v === "AUTO") return "auto";
  if (v === true || v === "true") return "stereo";
  return "mono";
}
function writeStereoMode(sel) {
  return sel === "auto" ? "auto" : sel === "stereo";
}

function renderConfigBlocks() {
  configBlocksEl.innerHTML = "";
  for (const blk of CONFIG_SCHEMA) {
    const present = configBlockObj(configModel, blk.block, false) !== undefined;
    const details = document.createElement("details");
    details.className = "config-block";
    details.dataset.block = blk.block;
    if (blk.open) details.open = true;

    const summary = document.createElement("summary");
    summary.textContent = blk.label;
    if (!present) {
      const tag = document.createElement("span");
      tag.className = "config-block-absent";
      tag.textContent = "not in file";
      summary.appendChild(tag);
    }
    details.appendChild(summary);

    const grid = document.createElement("div");
    grid.className = "config-fields";
    for (const field of blk.fields) {
      grid.appendChild(renderConfigField(blk, field));
    }
    details.appendChild(grid);
    configBlocksEl.appendChild(details);
  }
}

function renderConfigField(blk, field) {
  const wrap = document.createElement("label");
  wrap.className = "config-field";
  const span = document.createElement("span");
  span.textContent = field.label;
  if (field.help) span.title = field.help;
  wrap.appendChild(span);

  const cur = (configBlockObj(configModel, blk.block, false) || {})[field.k];
  let input;
  if (field.t === "bool") {
    input = document.createElement("input");
    input.type = "checkbox";
    input.checked = cur === undefined ? !!field.def : !!cur;
  } else if (field.t === "enum") {
    input = document.createElement("select");
    for (const [val, text] of field.options) {
      const o = document.createElement("option");
      o.value = val;
      o.textContent = text;
      input.appendChild(o);
    }
    input.value =
      blk.block === "stereo" && field.k === "mode"
        ? readStereoMode(cur === undefined ? field.def : cur)
        : cur === undefined
          ? field.def
          : String(cur);
  } else {
    input = document.createElement("input");
    input.type = "number";
    if (field.t === "mhz") input.step = "0.1";
    if (field.min != null) input.min = field.min;
    if (field.max != null) input.max = field.max;
    input.placeholder = fieldDisplayValue(field, field.def);
    input.value = fieldDisplayValue(field, cur);
  }
  if (field.help) input.title = field.help;

  const evt = field.t === "bool" || field.t === "enum" ? "change" : "input";
  input.addEventListener(evt, () => {
    let value;
    if (field.t === "bool") value = input.checked;
    else if (blk.block === "stereo" && field.k === "mode")
      value = writeStereoMode(input.value);
    else value = coerceConfigValue(field, input.value);

    const container = configBlockObj(configModel, blk.block, true);
    if (value === undefined) {
      delete container[field.k];
      pruneEmptyBlock(configModel, blk.block);
    } else {
      container[field.k] = value;
    }

    configTextarea.value = emitConfigYaml(configModel);
    markConfigDirty();
  });

  wrap.appendChild(input);
  return wrap;
}

// Hand-edit of the raw YAML: re-parse into the model and re-render fields.
configTextarea.addEventListener("input", () => {
  try {
    configModel = normalizeConfigModel(jsyaml.load(configTextarea.value) || {});
    configStatusEl.textContent = "";
    renderConfigBlocks();
    markConfigDirty();
  } catch (e) {
    configStatusEl.textContent = `YAML error: ${e.message}`;
  }
});

configValidateBtn.addEventListener("click", async () => {
  configStatusEl.textContent = "validating…";
  try {
    const result = await apiCall("config", "validate", "POST", {
      yaml: configTextarea.value,
    });
    renderResult(configResultEl, result);
    configStatusEl.textContent = "";
  } catch (e) {
    configStatusEl.textContent = e.message;
  }
});

configSaveBtn.addEventListener("click", async () => {
  configStatusEl.textContent = "saving…";
  try {
    const result = await apiCall("config", "raw", "PUT", {
      yaml: configTextarea.value,
    });
    renderResult(configResultEl, result);
    configStatusEl.textContent = result.ok ? "saved" : "rejected, not saved";
    if (result.ok) {
      configSavedYaml = configTextarea.value;
      markConfigClean();
    }
  } catch (e) {
    configStatusEl.textContent = e.message;
  }
});

configCancelBtn.addEventListener("click", () => {
  if (
    configDirty &&
    !window.confirm("Discard unsaved changes to the server config?")
  ) {
    return;
  }
  loadConfigFromYaml(configSavedYaml);
  configResultEl.innerHTML = "";
  configStatusEl.textContent = "reverted to the saved config";
});

// --- stations.yml: structured row editor --------------------------

const stationsSection = document.getElementById("stations-editor");
const stationsRowsEl = document.getElementById("stations-rows");
const stationAddBtn = document.getElementById("station-add");
const stationsCountEl = stationsSection.querySelector(".stations-count");
const stationsValidateBtn = stationsSection.querySelector(".validate-btn");
const stationsSaveBtn = stationsSection.querySelector(".save-btn");
const stationsCancelBtn = stationsSection.querySelector(".cancel-btn");
const stationsStatusEl = stationsSection.querySelector(".editor-status");
const stationsResultEl = stationsSection.querySelector(".editor-result");
const stationsYamlPeekEl = stationsSection.querySelector(".stations-yaml-text");

// Set while the raw-YAML textarea is being hand-edited, so refreshYamlPeek()
// doesn't overwrite what the user is typing with the re-emitted form.
let stationsRawEditing = false;

// Source of truth for the table. Each entry:
//   { label, freqHz, port|null, enabled, stereo, rds, squelch, agc, extra }
// stereo:            undefined (use config default) | true | false | "auto"
// rds/squelch/agc:   undefined (use config default) | bool | {mapping}
//                    (agc has no `true` form; a mapping or `false`)
// extra:             any keys the row editor doesn't model, kept verbatim
let stations = [];
let stationsDirty = false;
// The YAML the table was last loaded from (server on login, or the
// re-emitted text after a successful save) -- Cancel reloads from this.
let stationsSavedYaml = "";

function markStationsDirty() {
  stationsDirty = true;
  refreshStationsCount();
  refreshYamlPeek();
}

function markStationsClean() {
  stationsDirty = false;
  refreshStationsCount();
  refreshYamlPeek();
}

function refreshStationsCount() {
  const on = stations.filter((s) => s.enabled).length;
  stationsCountEl.textContent =
    `${stations.length} station${stations.length === 1 ? "" : "s"}` +
    (on !== stations.length ? ` (${stations.length - on} off)` : "") +
    (stationsDirty ? "  ·  unsaved changes" : "");
  stationsCountEl.classList.toggle("is-dirty", stationsDirty);
  stationsCancelBtn.disabled = !stationsDirty;
}

function refreshYamlPeek() {
  if (stationsRawEditing) return;
  stationsYamlPeekEl.value = emitStationsYaml(stations);
}

function normStereo(v) {
  if (v === undefined || v === null) return undefined;
  if (typeof v === "string") return v.toLowerCase() === "auto" ? "auto" : v.toLowerCase() === "true";
  return !!v;
}

const STATION_KNOWN_KEYS = ["label", "freq", "port", "enabled", "stereo", "rds", "squelch", "agc"];

function stationsFromDoc(doc) {
  const list = Array.isArray(doc.stations) ? doc.stations : [];
  return list.map((raw) => {
    const s = raw && typeof raw === "object" ? raw : {};
    const extra = {};
    for (const k of Object.keys(s)) {
      if (!STATION_KNOWN_KEYS.includes(k)) extra[k] = s[k];
    }
    return {
      label: s.label != null ? String(s.label) : "",
      freqHz: Number(s.freq) || 0,
      port: s.port != null ? Number(s.port) : null,
      enabled: s.enabled === undefined ? true : !!s.enabled,
      stereo: normStereo(s.stereo),
      rds: s.rds,
      squelch: s.squelch,
      agc: s.agc,
      extra,
    };
  });
}

function loadStationsFromYaml(yamlText) {
  let doc;
  try {
    doc = jsyaml.load(yamlText) || {};
  } catch (e) {
    stationsStatusEl.textContent = `could not parse stations.yml: ${e.message}`;
    stations = [];
    renderStations();
    return;
  }
  stationsSavedYaml = yamlText;
  stations = stationsFromDoc(doc);
  markStationsClean();
  renderStations();
}

// Hand-edit of the raw YAML: re-parse into the table model without
// touching the saved baseline (that's what Cancel restores).
stationsYamlPeekEl.addEventListener("input", () => {
  let doc;
  try {
    doc = jsyaml.load(stationsYamlPeekEl.value) || {};
  } catch (e) {
    stationsStatusEl.textContent = `YAML error: ${e.message}`;
    return;
  }
  stationsStatusEl.textContent = "";
  stationsRawEditing = true;
  stations = stationsFromDoc(doc);
  renderStations();
  markStationsDirty();
  stationsRawEditing = false;
});

// --- YAML emit (hand-rolled so the file stays block-style, one blank
// line between stations, keys in a stable order). Mapping-valued
// overrides fall back to js-yaml flow style. ---------------------------

function yamlScalar(s) {
  s = String(s);
  // Bare where it's unambiguous; otherwise double-quote (valid YAML).
  if (
    /^[A-Za-z0-9][\w .+\-/&()'!?]*$/.test(s) &&
    !/^(true|false|null|yes|no|on|off|~)$/i.test(s)
  ) {
    return s;
  }
  return JSON.stringify(s);
}

function yamlInlineValue(v) {
  if (v === true || v === false || typeof v === "number") return String(v);
  if (typeof v === "string") return yamlScalar(v);
  // object / array -> compact flow style on one line
  return jsyaml.dump(v, { flowLevel: 0, lineWidth: -1 }).trim();
}

function emitStationsYaml(list) {
  const lines = ["stations:"];
  list.forEach((st, i) => {
    if (i) lines.push("");
    lines.push(`- label: ${yamlScalar(st.label)}`);
    lines.push(`  freq: ${st.freqHz}`);
    if (st.port != null) lines.push(`  port: ${st.port}`);
    if (!st.enabled) lines.push(`  enabled: false`);
    if (st.stereo !== undefined) {
      lines.push(`  stereo: ${st.stereo === "auto" ? "auto" : st.stereo}`);
    }
    for (const key of ["rds", "squelch", "agc"]) {
      if (st[key] === undefined) continue;
      lines.push(`  ${key}: ${yamlInlineValue(st[key])}`);
    }
    for (const [k, v] of Object.entries(st.extra || {})) {
      lines.push(`  ${k}: ${yamlInlineValue(v)}`);
    }
  });
  return lines.join("\n") + "\n";
}

// --- row rendering ----------------------------------------------------

function mhzFromHz(hz) {
  // Full precision -- a carrier off the 100 kHz raster (or a hand-tuned
  // freq) must survive an edit untouched, so no fixed decimal count.
  return hz ? String(hz / 1e6) : "";
}

// undefined | bool | {mapping}  <->  select value + optional raw field.
// "default" => undefined, "on" => true, "off" => false, "custom" => the
// raw flow-YAML in the sibling text input (parsed back on emit).
function overrideSelectValue(v) {
  if (v === undefined) return "default";
  if (v === true) return "on";
  if (v === false) return "off";
  return "custom";
}

function makeOverrideControl(labelText, st, key, opts) {
  const wrap = document.createElement("label");
  wrap.className = "adv-field";
  const span = document.createElement("span");
  span.textContent = labelText;
  wrap.appendChild(span);

  const sel = document.createElement("select");
  for (const [val, text] of opts) {
    const o = document.createElement("option");
    o.value = val;
    o.textContent = text;
    sel.appendChild(o);
  }
  sel.value = overrideSelectValue(st[key]);
  wrap.appendChild(sel);

  const raw = document.createElement("input");
  raw.type = "text";
  raw.className = "adv-raw";
  raw.placeholder = "{target: 0.3, max_gain_db: 10}";
  raw.hidden = sel.value !== "custom";
  if (sel.value === "custom") raw.value = yamlInlineValue(st[key]);
  wrap.appendChild(raw);

  sel.addEventListener("change", () => {
    if (sel.value === "default") st[key] = undefined;
    else if (sel.value === "on") st[key] = true;
    else if (sel.value === "off") st[key] = false;
    else {
      st[key] = st[key] && typeof st[key] === "object" ? st[key] : {};
      raw.value = yamlInlineValue(st[key]);
    }
    raw.hidden = sel.value !== "custom";
    markStationsDirty();
  });
  raw.addEventListener("input", () => {
    try {
      const parsed = jsyaml.load(raw.value);
      st[key] = parsed;
      raw.classList.remove("is-bad");
    } catch (_) {
      raw.classList.add("is-bad");
    }
    markStationsDirty();
  });
  return wrap;
}

function renderStations() {
  stationsRowsEl.innerHTML = "";
  stations.forEach((st, idx) => {
    const tr = document.createElement("tr");
    tr.className = "station-row";
    if (!st.enabled) tr.classList.add("is-off");

    // On
    const onTd = document.createElement("td");
    onTd.className = "col-on";
    const onCb = document.createElement("input");
    onCb.type = "checkbox";
    onCb.checked = st.enabled;
    onCb.addEventListener("change", () => {
      st.enabled = onCb.checked;
      tr.classList.toggle("is-off", !st.enabled);
      markStationsDirty();
    });
    onTd.appendChild(onCb);
    tr.appendChild(onTd);

    // Label
    const labelTd = document.createElement("td");
    labelTd.className = "col-label";
    const labelIn = document.createElement("input");
    labelIn.type = "text";
    labelIn.value = st.label;
    labelIn.addEventListener("input", () => {
      st.label = labelIn.value;
      markStationsDirty();
    });
    labelTd.appendChild(labelIn);
    tr.appendChild(labelTd);

    // Frequency (MHz in the field, Hz in the model)
    const freqTd = document.createElement("td");
    freqTd.className = "col-freq";
    const freqIn = document.createElement("input");
    freqIn.type = "number";
    freqIn.step = "0.1";
    freqIn.min = "50";
    freqIn.value = mhzFromHz(st.freqHz);
    freqIn.addEventListener("input", () => {
      const mhz = parseFloat(freqIn.value);
      st.freqHz = Number.isFinite(mhz) ? Math.round(mhz * 1e6) : 0;
      markStationsDirty();
    });
    freqTd.appendChild(freqIn);
    tr.appendChild(freqTd);

    // Port
    const portTd = document.createElement("td");
    portTd.className = "col-port";
    const portIn = document.createElement("input");
    portIn.type = "number";
    portIn.min = "1025";
    portIn.max = "65535";
    portIn.value = st.port == null ? "" : st.port;
    portIn.addEventListener("input", () => {
      const p = parseInt(portIn.value, 10);
      st.port = Number.isFinite(p) ? p : null;
      markStationsDirty();
    });
    portTd.appendChild(portIn);
    tr.appendChild(portTd);

    // Stereo
    const stereoTd = document.createElement("td");
    stereoTd.className = "col-stereo";
    const stereoSel = document.createElement("select");
    for (const [val, text] of [
      ["default", "default"],
      ["mono", "mono"],
      ["stereo", "stereo"],
      ["auto", "auto"],
    ]) {
      const o = document.createElement("option");
      o.value = val;
      o.textContent = text;
      stereoSel.appendChild(o);
    }
    stereoSel.value =
      st.stereo === undefined
        ? "default"
        : st.stereo === "auto"
          ? "auto"
          : st.stereo
            ? "stereo"
            : "mono";
    stereoSel.addEventListener("change", () => {
      st.stereo =
        stereoSel.value === "default"
          ? undefined
          : stereoSel.value === "auto"
            ? "auto"
            : stereoSel.value === "stereo";
      markStationsDirty();
    });
    stereoTd.appendChild(stereoSel);
    tr.appendChild(stereoTd);

    // Advanced toggle
    const advTd = document.createElement("td");
    advTd.className = "col-adv";
    const advBtn = document.createElement("button");
    advBtn.type = "button";
    advBtn.className = "adv-toggle";
    const hasOverrides =
      st.rds !== undefined || st.squelch !== undefined || st.agc !== undefined;
    advBtn.textContent = hasOverrides ? "⋯ *" : "⋯";
    advBtn.title = "Per-station RDS / squelch / AGC overrides";
    advTd.appendChild(advBtn);
    tr.appendChild(advTd);

    // Reorder
    const ordTd = document.createElement("td");
    ordTd.className = "col-ord";
    const upBtn = document.createElement("button");
    upBtn.type = "button";
    upBtn.textContent = "↑";
    upBtn.disabled = idx === 0;
    upBtn.addEventListener("click", () => moveStation(idx, -1));
    const downBtn = document.createElement("button");
    downBtn.type = "button";
    downBtn.textContent = "↓";
    downBtn.disabled = idx === stations.length - 1;
    downBtn.addEventListener("click", () => moveStation(idx, 1));
    ordTd.append(upBtn, downBtn);
    tr.appendChild(ordTd);

    // Delete
    const delTd = document.createElement("td");
    delTd.className = "col-del";
    const delBtn = document.createElement("button");
    delBtn.type = "button";
    delBtn.className = "del-btn";
    delBtn.textContent = "×";
    delBtn.title = "Remove this station";
    delBtn.addEventListener("click", () => {
      stations.splice(idx, 1);
      markStationsDirty();
      renderStations();
    });
    delTd.appendChild(delBtn);
    tr.appendChild(delTd);

    stationsRowsEl.appendChild(tr);

    // Advanced panel (its own row, spanning the table)
    const advRow = document.createElement("tr");
    advRow.className = "station-adv";
    advRow.hidden = true;
    const advCell = document.createElement("td");
    advCell.colSpan = 8;
    advCell.append(
      makeOverrideControl("RDS", st, "rds", [
        ["default", "default"],
        ["on", "on"],
        ["off", "off"],
        ["custom", "custom…"],
      ]),
      makeOverrideControl("Squelch", st, "squelch", [
        ["default", "default"],
        ["on", "on"],
        ["off", "off"],
        ["custom", "custom…"],
      ]),
      makeOverrideControl("AGC", st, "agc", [
        ["default", "default"],
        ["off", "off"],
        ["custom", "custom…"],
      ]),
    );
    advRow.appendChild(advCell);
    stationsRowsEl.appendChild(advRow);

    advBtn.addEventListener("click", () => {
      advRow.hidden = !advRow.hidden;
      advBtn.classList.toggle("is-open", !advRow.hidden);
    });
  });

  refreshStationsCount();
  refreshYamlPeek();
}

function moveStation(idx, delta) {
  const j = idx + delta;
  if (j < 0 || j >= stations.length) return;
  [stations[idx], stations[j]] = [stations[j], stations[idx]];
  markStationsDirty();
  renderStations();
}

// rx requires an explicit port on every station (no auto-assignment,
// see resolve.hpp) -- never hand back a falsy port.
const FALLBACK_PORT_BASE = 7355;

function nextFreePort() {
  const max = stations.reduce((m, s) => Math.max(m, s.port || 0), 0);
  return max ? max + 1 : FALLBACK_PORT_BASE;
}

stationAddBtn.addEventListener("click", () => {
  stations.push({
    label: "",
    freqHz: 0,
    port: nextFreePort(),
    enabled: true,
    stereo: undefined,
    rds: undefined,
    squelch: undefined,
    agc: undefined,
    extra: {},
  });
  markStationsDirty();
  renderStations();
  stationsRowsEl.lastElementChild.previousElementSibling
    ?.querySelector(".col-label input")
    ?.focus();
});

stationsValidateBtn.addEventListener("click", async () => {
  stationsStatusEl.textContent = "validating…";
  try {
    const result = await apiCall("stations", "validate", "POST", {
      yaml: emitStationsYaml(stations),
    });
    renderResult(stationsResultEl, result);
    stationsStatusEl.textContent = "";
  } catch (e) {
    stationsStatusEl.textContent = e.message;
  }
});

stationsSaveBtn.addEventListener("click", async () => {
  stationsStatusEl.textContent = "saving…";
  try {
    const result = await apiCall("stations", "raw", "PUT", {
      yaml: emitStationsYaml(stations),
    });
    renderResult(stationsResultEl, result);
    stationsStatusEl.textContent = result.ok ? "saved" : "rejected, not saved";
    if (result.ok) {
      stationsSavedYaml = emitStationsYaml(stations);
      markStationsClean();
    }
  } catch (e) {
    stationsStatusEl.textContent = e.message;
  }
});

stationsCancelBtn.addEventListener("click", () => {
  if (
    stationsDirty &&
    !window.confirm("Discard unsaved changes to the station list?")
  ) {
    return;
  }
  loadStationsFromYaml(stationsSavedYaml);
  stationsResultEl.innerHTML = "";
  stationsStatusEl.textContent = "reverted to the saved list";
});

// --- login / load ---------------------------------------------------

async function loadAll() {
  authStatusEl.textContent = "logging in…";
  try {
    const cfg = await apiCall("config", "raw", "GET");
    loadConfigFromYaml(cfg.yaml);
    const st = await apiCall("stations", "raw", "GET");
    loadStationsFromYaml(st.yaml);
    showLoggedIn();
    // Resume live polling if the Signal tab is the one we came back to.
    if (document.querySelector(".tab-btn.is-active")?.dataset.tab === "signal")
      startSignalPolling();
  } catch (e) {
    // A 401 already reset to the logged-out state via authFetch; for any
    // other failure (e.g. no rx daemon configured) leave the form up with
    // the error instead of discarding a password that was actually fine.
    authStatusEl.textContent = e.message;
  }
}

loginForm.addEventListener("submit", (event) => {
  event.preventDefault();
  adminPassword = passwordInput.value;
  sessionStorage.setItem(SESSION_KEY, adminPassword);
  loadAll();
});

logoutBtn.addEventListener("click", () => showLoggedOut());

// A password surviving from an earlier reload in this tab -- try it
// straight away instead of making the user re-type it.
if (adminPassword) loadAll();

// --- live signal tab ---------------------------------------------------
// GET /admin/api/status/full -> the receiver's complete status frame
// (rx status_json run with StatusPublic::all()), regardless of the
// status.public.* visibility toggles that trim the public /api/status.
// Polled only while the Signal tab is visible and we're logged in.

const signalStatusEl = document.querySelector(".signal-live-status");
const signalHealthEl = document.querySelector(".rx-health-admin");
const signalRowsEl = document.getElementById("signal-rows");
const SIGNAL_POLL_MS = 2500;
let signalTimer = null;

function signalNum(v, digits = 0) {
  return typeof v === "number" && Number.isFinite(v) ? v.toFixed(digits) : "—";
}

function signalRds(rds) {
  if (!rds) return "—";
  const bits = [];
  if (rds.ps) bits.push(rds.ps.trim());
  if (typeof rds.pi === "number")
    bits.push("PI " + rds.pi.toString(16).toUpperCase().padStart(4, "0"));
  if (rds.pty_name) bits.push(rds.pty_name);
  if (typeof rds.ber === "number") bits.push(`${(rds.ber * 100).toFixed(0)}% blk`);
  if (rds.rt) bits.push(`“${rds.rt.trim()}”`);
  if (!bits.length) return rds.lock ? "locked" : "acquiring";
  return bits.join(" · ");
}

function renderSignalHealth(sdr, decode) {
  if (!sdr) {
    signalHealthEl.hidden = true;
    return;
  }
  const bits = [`SDR ${sdr.link_up ? "up" : "DOWN"}`, `RT ${signalNum(sdr.rt_percent)}%`];
  if (typeof sdr.hardwaregain_db === "number") {
    const mode =
      sdr.gain_mode && sdr.gain_mode !== "manual" ? ` (${sdr.gain_mode})` : "";
    bits.push(`gain ${sdr.hardwaregain_db.toFixed(1)} dB${mode}`);
  }
  if (typeof sdr.peak_dbfs === "number") bits.push(`peak ${sdr.peak_dbfs.toFixed(1)} dBFS`);
  if (typeof sdr.clip_ppm === "number" && sdr.clip_ppm >= 1)
    bits.push(`clip ${sdr.clip_ppm.toFixed(0)} ppm`);
  if (typeof sdr.overruns === "number") bits.push(`${sdr.overruns} overruns (lifetime)`);
  if (decode && decode.listener_gated)
    bits.push(`listener-gated${decode.controlled ? "" : " · fail-open"}`);
  signalHealthEl.textContent = bits.join("  ·  ");
  signalHealthEl.hidden = false;
}

function renderSignalTable(stations) {
  signalRowsEl.innerHTML = "";
  for (const s of stations || []) {
    let stereo = !s.stereo
      ? "mono"
      : typeof s.stereo_frac === "number"
        ? s.stereo_frac > 0.5
          ? "stereo"
          : `blend ${s.stereo_frac.toFixed(2)}`
        : "on";
    if (typeof s.pilot_db === "number")
      stereo += ` · pilot ${s.pilot_db.toFixed(0)} dBc${s.pilot_lock ? " lock" : ""}`;
    const cells = [
      s.label || "",
      s.port == null ? "—" : s.port,
      s.idle ? "idle" : "live",
      signalNum(s.rf_dbfs),
      signalNum(s.snr_db),
      s.squelch_open === false ? "muted" : signalNum(s.squelch_metric_db),
      signalNum(s.agc_gain_db),
      signalNum(s.afc_hz),
      typeof s.high_cut_hz === "number" ? (s.high_cut_hz / 1000).toFixed(1) : "—",
      signalNum(s.multipath),
      s.demod || "—",
      stereo,
      signalRds(s.rds),
    ];
    const tr = document.createElement("tr");
    if (s.idle) tr.className = "is-idle";
    for (const c of cells) {
      const td = document.createElement("td");
      td.textContent = String(c);
      tr.appendChild(td);
    }
    signalRowsEl.appendChild(tr);
  }
}

async function pollSignalOnce() {
  try {
    const response = await authFetch("/admin/api/status/full");
    if (response.status === 503) {
      signalStatusEl.textContent =
        "no rx daemon URL configured for this webui instance (--rx-url)";
      return;
    }
    if (!response.ok) {
      signalStatusEl.textContent = `status unavailable (${response.status})`;
      return;
    }
    const data = await response.json();
    renderSignalHealth(data.sdr, data.decode);
    renderSignalTable(data.stations);
    signalStatusEl.textContent = `updated ${new Date().toLocaleTimeString()}`;
  } catch (e) {
    // authFetch already handled a 401 (back to the login form); anything
    // else just shows on the status line and the timer tries again.
    signalStatusEl.textContent = e.message;
  }
}

function startSignalPolling() {
  if (signalTimer || !adminPassword || contentEl.hidden) return;
  pollSignalOnce();
  signalTimer = setInterval(pollSignalOnce, SIGNAL_POLL_MS);
}

function stopSignalPolling() {
  if (signalTimer) {
    clearInterval(signalTimer);
    signalTimer = null;
  }
}

// --- tabs ----------------------------------------------------------
// Stations (list + band scan) | Server config (config.yml textarea).
// Switching just toggles visibility -- nothing is unloaded, so unsaved
// station edits survive a hop to the config tab and back.

const TAB_KEY = "feedmyfm-admin-tab";
const tabBtns = Array.from(document.querySelectorAll(".tab-btn"));
const tabPanels = Array.from(document.querySelectorAll(".tab-panel"));

function showTab(name) {
  for (const b of tabBtns) b.classList.toggle("is-active", b.dataset.tab === name);
  for (const pnl of tabPanels) pnl.hidden = pnl.dataset.tab !== name;
  if (name === "signal") startSignalPolling();
  else stopSignalPolling();
  try {
    sessionStorage.setItem(TAB_KEY, name);
  } catch (_) {
    /* private mode -- tab just won't persist */
  }
}

for (const b of tabBtns) {
  b.addEventListener("click", () => showTab(b.dataset.tab));
}

let startTab = "stations";
try {
  const saved = sessionStorage.getItem(TAB_KEY);
  if (saved && tabBtns.some((b) => b.dataset.tab === saved)) startTab = saved;
} catch (_) {
  /* ignore */
}
showTab(startTab);

// --- band scan -------------------------------------------------------
// GET /admin/api/scan -> the rx daemon's passive periodogram of its
// current RF span (see rx/src/scan.hpp). Drawn as an SVG spectrum with a
// tick per detected carrier; clicking a carrier that isn't already in
// stations.yml appends a station row for the operator to review + save.

const scanBtn = document.getElementById("scan-btn");
const scanAddAllBtn = document.getElementById("scan-add-all");
const scanStatusEl = document.querySelector(".bandscan-status");
const scanPlotEl = document.querySelector(".bandscan-plot");

// Carriers from the most recent scan, kept so "Add all detected
// carriers" can act on them after the plot is drawn.
let lastScanCarriers = [];

function escapeHtml(s) {
  return s.replace(/[&<>"']/g, (c) => ({
    "&": "&amp;",
    "<": "&lt;",
    ">": "&gt;",
    '"': "&quot;",
    "'": "&#39;",
  })[c]);
}

function configuredStationFreqs() {
  return stations.map((s) => s.freqHz).filter(Boolean);
}

// Freqs of entries with `enabled: false`. The rx daemon drops these
// before Plan is built, so GET /api/scan still flags their carrier as
// `configured` (see rx/src/scan.hpp) -- the plot uses this to mark them.
function disabledStationFreqs() {
  return stations.filter((s) => !s.enabled).map((s) => s.freqHz).filter(Boolean);
}

// A carrier is "already in the list" if some station sits within 50 kHz
// of it. Deliberately tighter than the rx scan's own +/-90 kHz
// "configured" tolerance (see isDisabledFreq below) -- this is just
// client-side de-dup for the Add/Add-all buttons, not a match against rx.
function freqAlreadyListed(freqHz) {
  return configuredStationFreqs().some((f) => Math.abs(f - freqHz) < 50000);
}

function blankStation(freqHz, label) {
  return {
    label: label != null ? label : (freqHz / 1e6).toFixed(1),
    freqHz,
    port: nextFreePort(),
    enabled: true,
    stereo: undefined,
    rds: undefined,
    squelch: undefined,
    agc: undefined,
    extra: {},
  };
}

function addStationAtFreq(freqHz) {
  if (freqAlreadyListed(freqHz)) {
    scanStatusEl.textContent = `${(freqHz / 1e6).toFixed(1)} MHz is already in the list`;
    return;
  }
  stations.push(blankStation(freqHz));
  markStationsDirty();
  renderStations();
  refreshScanAddAll();
  stationsSection.scrollIntoView({ behavior: "smooth", block: "center" });
  scanStatusEl.textContent = `added ${(freqHz / 1e6).toFixed(1)} MHz — review, Validate, and Save below`;
}

// Bulk: every detected carrier not already in the list becomes a row.
// Additive and non-destructive -- existing rows (labels, ports, disabled
// entries, overrides) are untouched.
function addAllScanCarriers() {
  const fresh = lastScanCarriers
    .map((c) => c.freq_hz)
    .filter((f) => !freqAlreadyListed(f));
  if (!fresh.length) {
    scanStatusEl.textContent = "every detected carrier is already in the list";
    return;
  }
  for (const f of fresh) stations.push(blankStation(f));
  markStationsDirty();
  renderStations();
  refreshScanAddAll();
  stationsSection.scrollIntoView({ behavior: "smooth", block: "start" });
  scanStatusEl.textContent =
    `added ${fresh.length} carrier${fresh.length === 1 ? "" : "s"} — review labels/ports, then Validate & Save`;
}

// Enable "Add all" only when the last scan turned up carriers not yet
// in the list; keep the count on the label.
function refreshScanAddAll() {
  const n = lastScanCarriers
    .map((c) => c.freq_hz)
    .filter((f) => !freqAlreadyListed(f)).length;
  scanAddAllBtn.disabled = n === 0;
  scanAddAllBtn.textContent = n
    ? `Add ${n} detected carrier${n === 1 ? "" : "s"}`
    : "Add all detected carriers";
}

scanAddAllBtn.addEventListener("click", addAllScanCarriers);

function renderSpectrum(data) {
  const W = 960;
  const H = 260;
  const M = { l: 6, r: 6, t: 30, b: 18 };
  const plotW = W - M.l - M.r;
  const plotH = H - M.t - M.b;

  const fMin = data.center_freq_hz - data.samp_rate_hz / 2;
  const fMax = data.center_freq_hz + data.samp_rate_hz / 2;
  const xf = (f) => M.l + ((f - fMin) / (fMax - fMin)) * plotW;

  const psd = data.psd_dbfs;
  const floor = data.noise_floor_dbfs;
  let dbMax = -Infinity;
  for (const v of psd) if (v > dbMax) dbMax = v;
  let dbMin = floor - 8;
  if (dbMax - dbMin < 20) dbMax = dbMin + 20;
  const yv = (db) => {
    const c = Math.max(dbMin, Math.min(dbMax, db));
    return M.t + ((dbMax - c) / (dbMax - dbMin)) * plotH;
  };

  // PSD trace, decimated to one point per output pixel (column max).
  const n = psd.length;
  const pts = [];
  for (let px = 0; px < plotW; px++) {
    const i0 = Math.floor((px * n) / plotW);
    const i1 = Math.max(i0 + 1, Math.floor(((px + 1) * n) / plotW));
    let mx = -Infinity;
    for (let i = i0; i < i1; i++) if (psd[i] > mx) mx = psd[i];
    pts.push(`${(M.l + px).toFixed(1)},${yv(mx).toFixed(1)}`);
  }

  const parts = [`<svg viewBox="0 0 ${W} ${H}" xmlns="http://www.w3.org/2000/svg">`];

  // frequency grid + labels
  for (let k = 0; k <= 8; k++) {
    const f = fMin + (k / 8) * (fMax - fMin);
    const x = xf(f).toFixed(1);
    parts.push(`<line class="grid" x1="${x}" y1="${M.t}" x2="${x}" y2="${M.t + plotH}"/>`);
    parts.push(
      `<text class="axis" x="${x}" y="${H - 6}" text-anchor="${
        k === 0 ? "start" : k === 8 ? "end" : "middle"
      }">${(f / 1e6).toFixed(1)}</text>`,
    );
  }

  // configured stations (faint ticks along the bottom); disabled ones get
  // a taller red dotted tick so they stand out from live carriers.
  const configured = configuredStationFreqs();
  const disabledFreqs = disabledStationFreqs();
  const isDisabledFreq = (f) => disabledFreqs.some((d) => Math.abs(d - f) <= 90000);
  for (const f of configured) {
    if (f < fMin || f > fMax) continue;
    const x = xf(f).toFixed(1);
    const y1 = isDisabledFreq(f) ? M.t + plotH - 16 : M.t + plotH - 8;
    const cls = isDisabledFreq(f) ? "station disabled" : "station";
    parts.push(`<line class="${cls}" x1="${x}" y1="${y1}" x2="${x}" y2="${M.t + plotH}"/>`);
  }

  // noise floor + PSD
  const yf = yv(floor).toFixed(1);
  parts.push(`<line class="floor" x1="${M.l}" y1="${yf}" x2="${W - M.r}" y2="${yf}"/>`);
  parts.push(`<polyline class="psd" points="${pts.join(" ")}"/>`);
  parts.push(
    `<text class="axis" x="${M.l}" y="${M.t - 18}">dBFS ${dbMax.toFixed(0)} … ${dbMin.toFixed(0)}` +
      `   noise floor ${floor.toFixed(0)}</text>`,
  );

  // detected carriers -- dashed vertical marker, frequency label above
  data.carriers.forEach((c, i) => {
    if (c.freq_hz < fMin || c.freq_hz > fMax) return;
    const x = xf(c.freq_hz).toFixed(1);
    const dis = c.configured && isDisabledFreq(c.freq_hz);
    const cls = c.configured
      ? dis
        ? "carrier configured disabled"
        : "carrier configured"
      : "carrier new";
    const labelY = M.t - 6 - (i % 2) * 11;
    parts.push(
      `<g class="${cls}" data-freq="${c.freq_hz}">` +
        `<title>${(c.freq_hz / 1e6).toFixed(1)} MHz  ${c.power_dbfs.toFixed(0)} dBFS` +
        `${c.configured ? ` (${escapeHtml(c.label || "configured")}${dis ? " — disabled" : ""})` : " — click to add"}</title>` +
        `<line x1="${x}" y1="${M.t}" x2="${x}" y2="${M.t + plotH}"/>` +
        `<text x="${x}" y="${labelY}">${(c.freq_hz / 1e6).toFixed(1)}</text>` +
        `</g>`,
    );
  });

  // Hover crosshair: a vertical line that tracks the mouse with a
  // frequency + level readout, so a weak carrier the detector missed can
  // still be eyeballed and its frequency read off directly. Clicking the
  // plot adds a station at the pointed-at frequency (snapped to 100 kHz).
  parts.push(
    `<g class="cursor" aria-hidden="true">` +
      `<line x1="0" y1="${M.t}" x2="0" y2="${M.t + plotH}"/>` +
      `<circle cx="0" cy="0" r="2.5"/>` +
      `<text x="0" y="${M.t + 10}"></text>` +
      `</g>`,
  );

  parts.push(`</svg>`);

  // detected-carrier list under the plot
  const rows = data.carriers
    .slice()
    .sort((a, b) => a.freq_hz - b.freq_hz)
    .map((c) => {
      const mhz = (c.freq_hz / 1e6).toFixed(1);
      const lvl = `${c.power_dbfs.toFixed(0)} dBFS`;
      if (c.configured) {
        const dis = isDisabledFreq(c.freq_hz);
        const tag = (c.label ? escapeHtml(c.label) : "configured") + (dis ? " (disabled)" : "");
        return (
          `<li class="carrier-row configured${dis ? " disabled" : ""}">` +
          `<span class="cr-freq">${mhz} MHz</span>` +
          `<span class="cr-level">${lvl}</span>` +
          `<span class="cr-tag">${tag}</span></li>`
        );
      }
      return (
        `<li class="carrier-row new" data-freq="${c.freq_hz}">` +
        `<span class="cr-freq">${mhz} MHz</span>` +
        `<span class="cr-level">${lvl}</span>` +
        `<button type="button" class="cr-add">add to stations.yml</button></li>`
      );
    })
    .join("");
  const listHtml = data.carriers.length
    ? `<ul class="carrier-list">${rows}</ul>`
    : `<p class="carrier-list-empty">no carriers above the detection threshold</p>`;

  scanPlotEl.innerHTML = parts.join("") + listHtml;
  scanPlotEl.hidden = false;

  const addFrom = (el) => addStationAtFreq(Number(el.dataset.freq));
  scanPlotEl.querySelectorAll("svg .carrier.new").forEach((el) =>
    el.addEventListener("click", (ev) => {
      ev.stopPropagation(); // don't also fire the plot-background click below
      addFrom(el);
    }),
  );
  scanPlotEl.querySelectorAll(".carrier-row.new").forEach((row) => {
    row
      .querySelector(".cr-add")
      .addEventListener("click", () => addFrom(row));
  });

  // --- hover crosshair + click-to-add on the plot itself ---------------
  const svg = scanPlotEl.querySelector("svg");
  const cursor = svg.querySelector(".cursor");
  const cursorLine = cursor.querySelector("line");
  const cursorDot = cursor.querySelector("circle");
  const cursorText = cursor.querySelector("text");
  const pt = svg.createSVGPoint();

  // clientX/Y -> SVG user units, correct through the viewBox scaling.
  function svgPoint(evt) {
    pt.x = evt.clientX;
    pt.y = evt.clientY;
    return pt.matrixTransform(svg.getScreenCTM().inverse());
  }

  function freqAtX(x) {
    return fMin + ((x - M.l) / plotW) * (fMax - fMin);
  }

  svg.addEventListener("mousemove", (evt) => {
    const p = svgPoint(evt);
    if (p.x < M.l || p.x > M.l + plotW) {
      cursor.classList.remove("on");
      return;
    }
    const f = freqAtX(p.x);
    const bin = Math.max(0, Math.min(n - 1, Math.floor(((f - fMin) / (fMax - fMin)) * n)));
    const x = xf(f);
    cursorLine.setAttribute("x1", x.toFixed(1));
    cursorLine.setAttribute("x2", x.toFixed(1));
    cursorDot.setAttribute("cx", x.toFixed(1));
    cursorDot.setAttribute("cy", yv(psd[bin]).toFixed(1));
    const nearRight = x > M.l + plotW - 96;
    cursorText.setAttribute("x", x.toFixed(1));
    cursorText.setAttribute("text-anchor", nearRight ? "end" : "start");
    cursorText.setAttribute("dx", nearRight ? "-5" : "5");
    cursorText.textContent = `${(f / 1e6).toFixed(2)} MHz   ${psd[bin].toFixed(0)} dBFS`;
    cursor.classList.add("on");
  });

  svg.addEventListener("mouseleave", () => cursor.classList.remove("on"));

  svg.addEventListener("click", (evt) => {
    const p = svgPoint(evt);
    if (p.x < M.l || p.x > M.l + plotW || p.y < M.t || p.y > M.t + plotH) return;
    addStationAtFreq(Math.round(freqAtX(p.x) / 1e5) * 1e5);
  });
}

async function runScan() {
  scanStatusEl.textContent = "scanning…";
  scanBtn.disabled = true;
  try {
    const response = await authFetch("/admin/api/scan");
    if (!response.ok) {
      let detail = `scan failed (${response.status})`;
      try {
        const j = await response.json();
        if (j.error || j.detail) detail = j.error || j.detail;
      } catch (_) {
        /* keep the status-code message */
      }
      throw new Error(detail);
    }
    const data = await response.json();
    lastScanCarriers = data.carriers || [];
    renderSpectrum(data);
    refreshScanAddAll();
    const news = data.carriers.filter((c) => !c.configured).length;
    scanStatusEl.textContent =
      `${data.carriers.length} carrier(s), ${news} not configured` +
      `  ·  noise floor ${data.noise_floor_dbfs.toFixed(0)} dBFS`;
  } catch (e) {
    scanStatusEl.textContent = e.message;
  } finally {
    scanBtn.disabled = false;
  }
}

scanBtn.addEventListener("click", runScan);

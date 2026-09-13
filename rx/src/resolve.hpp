// Resolve config.yml + stations.yml into a ready-to-run fmrx::Plan.
//
// This is the ONLY place the frequency-planning math and the config
// validation live -- the DSP code just executes the numbers this produces.
//
// Two classes of problem:
//   * hard errors  -> throw std::runtime_error. The receiver refuses to
//                     start.
//   * soft findings -> pushed onto ResolveResult::findings for the caller
//                      to print as `note:` lines (dropped out-of-band
//                      stations, tight guard bands, bin collisions, ...).
//
// The signal chain is deliberately narrow: radio.samp_rate is the only
// rate, there is no DDC stage or sub-band split, and audio resampling is
// integer decimation only (no rational resampler).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <initializer_list>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "plan.hpp"
#include "yaml.hpp"

namespace fmrx {

struct ResolveResult {
    Plan plan;
    std::vector<std::string> findings;
};

namespace detail {

inline std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        throw std::runtime_error("cannot open '" + path + "'");
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

[[noreturn]] inline void err(const std::string& msg) {
    throw std::runtime_error(msg);
}

// snprintf into a std::string -- for human-readable "12.3 kHz" messages.
template <typename... A>
inline std::string fmt(const char* f, A... args) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), f, args...);
    return std::string(buf);
}

inline void check_keys(const yaml::Value& m, const std::string& where,
                       std::initializer_list<const char*> allowed) {
    if (!m.is_map())
        err(where + ": expected a mapping");
    for (const auto& kv : m.items()) {
        bool ok = false;
        for (const char* a : allowed)
            if (kv.first == a) {
                ok = true;
                break;
            }
        if (!ok)
            err(where + ": unknown key '" + kv.first + "'");
    }
}

// Range check whose message names the offending key.
inline void bound(bool ok, const std::string& key, const std::string& what) {
    if (!ok)
        err(key + " " + what);
}

inline void nearest_wrapped_bin(double rel_freq, double bin_width, int n,
                                int& bin_idx, long long& k_wrap) {
    // Compare both integer neighbours of rel_freq/bin_width by actual Hz
    // distance so the seam at the wrapped Nyquist bin is handled correctly.
    const double raw = rel_freq / bin_width;
    const long long lo = static_cast<long long>(std::floor(raw));
    const long long hi = static_cast<long long>(std::ceil(raw));
    bool have = false;
    double best = 0.0;
    for (long long k : {lo, hi}) {
        const long long kw = (k < n / 2) ? k : k - n;
        const double dist = std::fabs(rel_freq - static_cast<double>(kw) * bin_width);
        if (!have || dist < best) {
            have = true;
            best = dist;
            k_wrap = kw;
            long long m = kw % n;
            if (m < 0)
                m += n;
            bin_idx = static_cast<int>(m);
        }
    }
}

// ---- audio leveling / squelch config (config `agc:` / `squelch:` blocks,
// with optional per-station overrides) ---------------------------------
struct AgcCfg {
    bool enabled = true;
    double target = 0.5;
    double max_gain_db = 40.0;
    double response_ms = 200.0;
};
struct SqCfg {
    bool enabled = false;
    double open_snr_db = 20.0;
    double hang_ms = 800.0;
};
struct StereoCfg {
    // Global default decode mode for stations that don't set their own
    // `stereo:` in stations.yml. Mono unless config.yml says otherwise.
    StereoMode mode = StereoMode::Mono;
    double pilot_threshold_db = -30.0; // 19 kHz pilot power, dBc of composite
    // Blend window on the calibrated, program-independent snr_db scale.
    // snr_db ~= recovered audio SNR; clean L-R separation needs ~50 dB, so
    // full stereo at 55 and full mono by 41. (Example measured spread:
    // fringe ~35, weak-but-usable ~46, clean ~65.)
    double blend_snr_lo_db = 41.0;     // full mono at/below this snr_db
    double blend_snr_hi_db = 55.0;     // full stereo at/above this snr_db
    bool pilot_pll = true;             // false -> normalise-and-square carrier
};

// A `stereo:` scalar -- "auto" (case-insensitive) -> Auto, otherwise a
// bool -> Stereo / Mono. Shared by the config.yml global and the
// stations.yml per-station override.
inline StereoMode parse_stereo_mode(const yaml::Value& v) {
    const std::string raw = v.as_string();
    if (raw == "auto" || raw == "Auto" || raw == "AUTO")
        return StereoMode::Auto;
    return v.as_bool() ? StereoMode::Stereo : StereoMode::Mono;
}
struct ListenerCfg {
    // Listener audio format, system-wide. Validated by the receiver so the
    // one config surface stays authoritative, but consumed by webui -- the
    // UDP output is byte-for-byte identical for both codecs.
    //
    // Read once at startup/reload: a change here needs an rx restart (or
    // `--watch` config reload) before webui's relay sees it via
    // GET /api/stations/active -- there's no webui-only way to flip codec
    // or bitrate.
    std::string codec = "pcm"; // pcm | aac
    int aac_bitrate_mono = 96000;
    int aac_bitrate_stereo = 128000;
};
struct SnrCfg {
    // snr_db on the status port is always the calibrated, program-
    // independent figure (guard-band noise-PSD estimate, ~2 elliptic IIRs
    // per station at channel_rate). There is no
    // opt-out: every SNR-driven feature (high-cut, stereo auto-blend, webui
    // weak-signal demotion) is tuned to that scale, and resolve_config
    // hard-errors if the plan can't fit the guard band.
    // squelch_metric_db still carries the raw proxy, for the squelch state
    // machine only.
    //
    // Absolute scaling for the calibrated figure. <= 0 means "use the
    // value fitted into the binary" (MonoStation::kSnrKNoiseDefault).
    // Override it here -- from `feedmyfm-rx --snr-cal` -- after any change
    // to channelizer.* / radio.samp_rate / fm.deviation / fm.tau /
    // audio.bandwidth|stop_bandwidth / fm.predemod_transition, to avoid
    // rebuilding. Really a per-chain constant, so set it on the global
    // block, not per station.
    double k_noise = 0.0;
};

inline void validate_agc(const AgcCfg& a, const std::string& where) {
    bound(a.target > 0.0 && a.target <= 1.0, where + ".target",
          "must be in (0, 1]");
    bound(a.max_gain_db >= 0.0, where + ".max_gain_db", "must be >= 0");
    bound(a.response_ms > 0.0, where + ".response_ms", "must be > 0");
}
inline void validate_sq(const SqCfg& q, const std::string& where) {
    bound(q.hang_ms >= 0.0, where + ".hang_ms", "must be >= 0");
}
inline void validate_stereo(const StereoCfg& s, const std::string& where) {
    bound(s.blend_snr_hi_db > s.blend_snr_lo_db, where + ".blend_snr_hi_db",
          "must be > blend_snr_lo_db");
}

// A block that may be absent (null), a bare bool scalar (enable/disable,
// all else default), or a mapping of overrides.
inline AgcCfg read_agc(const yaml::Value& m, AgcCfg base,
                       const std::string& where) {
    if (m.is_null())
        return base;
    if (m.is_scalar()) {
        base.enabled = m.as_bool();
        return base;
    }
    check_keys(m, where, {"enabled", "target", "max_gain_db", "response_ms"});
    if (m.has("enabled")) base.enabled = m.at("enabled").as_bool();
    if (m.has("target")) base.target = m.at("target").as_double();
    if (m.has("max_gain_db")) base.max_gain_db = m.at("max_gain_db").as_double();
    if (m.has("response_ms")) base.response_ms = m.at("response_ms").as_double();
    validate_agc(base, where);
    return base;
}
inline SqCfg read_sq(const yaml::Value& m, SqCfg base, const std::string& where) {
    if (m.is_null())
        return base;
    if (m.is_scalar()) {
        base.enabled = m.as_bool();
        return base;
    }
    check_keys(m, where, {"enabled", "open_snr_db", "hang_ms"});
    if (m.has("enabled")) base.enabled = m.at("enabled").as_bool();
    if (m.has("open_snr_db")) base.open_snr_db = m.at("open_snr_db").as_double();
    if (m.has("hang_ms")) base.hang_ms = m.at("hang_ms").as_double();
    validate_sq(base, where);
    return base;
}
inline StereoCfg read_stereo(const yaml::Value& m, StereoCfg base,
                             const std::string& where) {
    if (m.is_null())
        return base;
    if (m.is_scalar()) { // `stereo: auto` / `true` / `false` shorthand
        base.mode = parse_stereo_mode(m);
        return base;
    }
    check_keys(m, where,
               {"mode", "pilot_threshold_db", "blend_snr_lo_db",
                "blend_snr_hi_db", "pilot_pll"});
    if (m.has("mode"))
        base.mode = parse_stereo_mode(m.at("mode"));
    if (m.has("pilot_threshold_db"))
        base.pilot_threshold_db = m.at("pilot_threshold_db").as_double();
    if (m.has("blend_snr_lo_db"))
        base.blend_snr_lo_db = m.at("blend_snr_lo_db").as_double();
    if (m.has("blend_snr_hi_db"))
        base.blend_snr_hi_db = m.at("blend_snr_hi_db").as_double();
    if (m.has("pilot_pll"))
        base.pilot_pll = m.at("pilot_pll").as_bool();
    validate_stereo(base, where);
    return base;
}
inline ListenerCfg read_listener(const yaml::Value& m, ListenerCfg base,
                                 const std::string& where) {
    if (m.is_null())
        return base;
    check_keys(m, where, {"codec", "aac_bitrate_mono", "aac_bitrate_stereo"});
    if (m.has("codec")) base.codec = m.at("codec").as_string();
    if (m.has("aac_bitrate_mono"))
        base.aac_bitrate_mono = m.at("aac_bitrate_mono").as_int();
    if (m.has("aac_bitrate_stereo"))
        base.aac_bitrate_stereo = m.at("aac_bitrate_stereo").as_int();
    if (base.codec != "pcm" && base.codec != "aac")
        err(where + ".codec: must be 'pcm' or 'aac', got '" + base.codec + "'");
    bound(base.aac_bitrate_mono >= 32000 && base.aac_bitrate_mono <= 256000,
          where + ".aac_bitrate_mono", "must be 32000-256000");
    bound(base.aac_bitrate_stereo >= 32000 && base.aac_bitrate_stereo <= 256000,
          where + ".aac_bitrate_stereo", "must be 32000-256000");
    return base;
}
inline SnrCfg read_snr(const yaml::Value& m, SnrCfg base,
                       const std::string& where) {
    if (m.is_null())
        return base;
    if (m.is_scalar())
        return base; // legacy `snr: <bool>` -- calibrated SNR is always on now
    check_keys(m, where, {"calibrated", "k_noise"});
    // `calibrated` is still accepted so existing configs validate, but it is
    // ignored: snr_db is unconditionally the calibrated figure.
    // resolve_config emits a deprecation finding.
    if (m.has("k_noise")) base.k_noise = m.at("k_noise").as_double();
    return base;
}

struct ScanCfg {
    // Passive band scan (GET /api/scan): an averaged wideband periodogram
    // of the SDR's current span. The FFT only runs while a scan request is
    // pending, so `enabled` mostly gates whether the object is built at all.
    bool enabled = true;
    int nfft = 4096;          // FFT size -- power of two, 256..65536
    int average_ms = 700;     // accumulation window per scan
    double threshold_db = 8.0; // carrier detection margin over the noise floor
};

inline ScanCfg read_scan(const yaml::Value& m, ScanCfg base,
                         const std::string& where) {
    if (m.is_null())
        return base;
    if (m.is_scalar()) {
        base.enabled = m.as_bool();
        return base;
    }
    check_keys(m, where, {"enabled", "nfft", "average_ms", "threshold_db"});
    if (m.has("enabled")) base.enabled = m.at("enabled").as_bool();
    if (m.has("nfft")) base.nfft = m.at("nfft").as_int();
    if (m.has("average_ms")) base.average_ms = m.at("average_ms").as_int();
    if (m.has("threshold_db"))
        base.threshold_db = m.at("threshold_db").as_double();
    bound(base.nfft >= 256 && base.nfft <= 65536 &&
              (base.nfft & (base.nfft - 1)) == 0,
          where + ".nfft",
          fmt("must be a power of two in 256-65536, got %d", base.nfft));
    bound(base.average_ms >= 100 && base.average_ms <= 15000,
          where + ".average_ms", "must be 100-15000");
    bound(base.threshold_db > 0.0 && base.threshold_db <= 60.0,
          where + ".threshold_db", "must be in (0, 60]");
    return base;
}

struct DecodeCfg {
    // Listener-gated per-station decode. Off by default: every configured
    // station is decoded unconditionally, as it always was. On: the RT
    // loop skips a station's chain while webui's relay reports no live
    // listener for it (POST /active), reverting to decode-all if the
    // relay goes quiet.
    bool listener_gated = false;
};

inline DecodeCfg read_decode(const yaml::Value& m, DecodeCfg base,
                             const std::string& where) {
    if (m.is_null())
        return base;
    if (m.is_scalar()) {
        base.listener_gated = m.as_bool();
        return base;
    }
    check_keys(m, where, {"listener_gated"});
    if (m.has("listener_gated"))
        base.listener_gated = m.at("listener_gated").as_bool();
    return base;
}

struct RdsCfg {
    // RDS (Radio Data System) decode. This is the global default for every
    // station; a per-station `rds:` in stations.yml overrides it (same
    // pattern as agc / squelch). Off by default. Only actually runs where
    // channel_rate_hz >= 120000 and -- under decode.listener_gated -- the
    // station is in the active set.
    bool enabled = false;
};

// Absent (null) -> inherit `base`; a bare bool -> enable/disable; a mapping
// -> {enabled: bool}. Used both for the config.yml global and, with the
// global as `base`, for the per-station stations.yml override.
inline RdsCfg read_rds(const yaml::Value& m, RdsCfg base,
                       const std::string& where) {
    if (m.is_null())
        return base;
    if (m.is_scalar()) {
        base.enabled = m.as_bool();
        return base;
    }
    check_keys(m, where, {"enabled"});
    if (m.has("enabled"))
        base.enabled = m.at("enabled").as_bool();
    return base;
}

} // namespace detail

// Output of resolve_config(): `plan` already
// holds everything that doesn't need per-station-override resolution
// (sdr/channelizer/audio-rate/status/scan/decode -- see "fill Plan" at the
// end of resolve_config). The rest of these fields are config-section
// values with no home on Plan itself (Plan has no *global*
// fm_deviation/tau/etc., only each StationPlan's own copy) that
// resolve_stations() still needs in order to fill those in per station.
struct ConfigResolved {
    Plan plan;
    std::vector<std::string> findings;
    long long bin_width = 0;
    long long carson_bw = 0;
    int predemod_transition = 0;
    int fm_deviation = 0;
    double fm_tau = 0.0;
    int fm_decim = 0;
    int fm_intermediate = 0;
    int audio_bw = 0;
    int audio_stop = 0;
    double deemph_b0 = 0.0;
    double deemph_p1 = 0.0;
    int audio_decim = 0;
    std::string fm_demod = "discriminator";
    bool fm_declick = false;
    double fm_declick_sigma = 5.0;
    double fm_pll_bw_hz = 0.0;
    bool fm_afc = true;
    bool fm_limiter = true;
    detail::AgcCfg agc;
    detail::SqCfg sq;
    detail::StereoCfg stereo;
    detail::SnrCfg snr;
    detail::RdsCfg rds;
};

// The config.yml half of resolve_text(): everything up to (not
// including) stations.yml. `cfg` is already parsed
// and confirmed to be a mapping -- resolve_text() does that much itself,
// since it's shared shape-checking, not config-specific.
inline ConfigResolved resolve_config(const yaml::Value& cfg) {
    using detail::bound;
    using detail::check_keys;
    using detail::err;
    using detail::fmt;

    ConfigResolved cr;
    Plan& p = cr.plan;

    check_keys(cfg, "config.yml",
               {"sdr", "radio", "channelizer", "fm", "audio", "udp", "agc",
                "squelch", "stereo", "snr", "listener", "status", "scan",
                "decode", "rds"});

    // ---- sdr --------------------------------------------------------
    const yaml::Value& sdr = cfg.at("sdr");
    check_keys(sdr, "config.yml sdr", {"ip", "gain_db", "gain_mode", "agc"});
    const std::string sdr_ip = sdr.at("ip").as_string();
    const int gain_db = sdr.at("gain_db").as_int();
    bound(gain_db >= 0 && gain_db <= 73, "sdr.gain_db",
          fmt("must be 0-73, got %d", gain_db));

    // gain_mode selects the AD9361's own AGC, or
    // `auto_sw` for the software peak-headroom loop. `sdr.agc.*` is only
    // meaningful under `auto_sw`; under any other mode it is silently
    // ignored (neither parsed nor validated) so a config can carry a
    // tuned `agc` block while temporarily running a different gain_mode.
    const std::string sdr_gain_mode =
        sdr.has("gain_mode") ? sdr.at("gain_mode").as_string() : "manual";
    bound(sdr_gain_mode == "manual" || sdr_gain_mode == "slow_attack" ||
              sdr_gain_mode == "fast_attack" || sdr_gain_mode == "hybrid" ||
              sdr_gain_mode == "auto_sw",
          "sdr.gain_mode",
          "must be manual | slow_attack | fast_attack | hybrid | auto_sw, got '" +
              sdr_gain_mode + "'");

    double agc_target_dbfs = -9.0, agc_max_dbfs = -3.0, agc_min_gain_db = 0.0;
    double agc_step_db = 1.0, agc_hysteresis_db = 3.0, agc_clip_ppm_trip = 5.0;
    int agc_interval_ms = 2000;
    if (sdr_gain_mode == "auto_sw" && sdr.has("agc")) {
        const yaml::Value& a = sdr.at("agc");
        check_keys(a, "config.yml sdr.agc",
                   {"target_dbfs", "max_dbfs", "min_gain_db", "step_db",
                    "interval_ms", "hysteresis_db", "clip_ppm_trip"});
        if (a.has("target_dbfs")) agc_target_dbfs = a.at("target_dbfs").as_double();
        if (a.has("max_dbfs")) agc_max_dbfs = a.at("max_dbfs").as_double();
        if (a.has("min_gain_db")) agc_min_gain_db = a.at("min_gain_db").as_double();
        if (a.has("step_db")) agc_step_db = a.at("step_db").as_double();
        if (a.has("interval_ms")) agc_interval_ms = a.at("interval_ms").as_int();
        if (a.has("hysteresis_db")) agc_hysteresis_db = a.at("hysteresis_db").as_double();
        if (a.has("clip_ppm_trip")) agc_clip_ppm_trip = a.at("clip_ppm_trip").as_double();
    }
    if (sdr_gain_mode == "auto_sw") {
        bound(agc_max_dbfs < 0.0, "sdr.agc.max_dbfs", "must be < 0 dBFS");
        bound(agc_target_dbfs < agc_max_dbfs, "sdr.agc.target_dbfs",
              "must be below sdr.agc.max_dbfs (leave headroom)");
        bound(agc_min_gain_db >= 0.0 && agc_min_gain_db < gain_db, "sdr.agc.min_gain_db",
              fmt("must be 0 <= min_gain_db < sdr.gain_db (%d)", gain_db));
        bound(agc_step_db > 0.0, "sdr.agc.step_db", "must be > 0");
        bound(agc_interval_ms > 0, "sdr.agc.interval_ms", "must be > 0");
        bound(agc_hysteresis_db >= 0.0, "sdr.agc.hysteresis_db", "must be >= 0");
        bound(agc_clip_ppm_trip > 0.0, "sdr.agc.clip_ppm_trip", "must be > 0");
    }

    // ---- radio ----------------------------------------------------
    const yaml::Value& radio = cfg.at("radio");
    check_keys(radio, "config.yml radio", {"center_freq", "samp_rate"});
    const long long center_freq = radio.at("center_freq").as_i64();
    const long long samp_rate = radio.at("samp_rate").as_i64();
    bound(center_freq > 0, "radio.center_freq", "must be > 0");
    bound(samp_rate > 0, "radio.samp_rate", "must be > 0");
    const long long proc = samp_rate; // no separate processing rate

    // ---- channelizer --------------------------------------------
    const yaml::Value& ch = cfg.at("channelizer");
    check_keys(ch, "config.yml channelizer",
               {"num_channels", "oversample", "atten_db", "proto_semilen_m",
                "block_size"});
    const int num_channels = ch.at("num_channels").as_int();
    const int oversample = ch.at("oversample").as_int();
    const int atten_db = ch.at("atten_db").as_int();
    const int proto_semilen_m = ch.at("proto_semilen_m").as_int();
    const int block_size = ch.at("block_size").as_int();
    bound(num_channels > 0, "channelizer.num_channels", "must be > 0");
    bound((num_channels & (num_channels - 1)) == 0, "channelizer.num_channels",
          fmt("must be a power of 2, got %d", num_channels));
    bound(oversample >= 1, "channelizer.oversample", "must be >= 1");
    bound(num_channels % oversample == 0, "channelizer.oversample",
          fmt("(%d) must evenly divide channelizer.num_channels (%d)",
              oversample, num_channels));
    bound(atten_db > 0, "channelizer.atten_db", "must be > 0");
    bound(proto_semilen_m > 0, "channelizer.proto_semilen_m", "must be > 0");
    bound(block_size > 0, "channelizer.block_size", "must be > 0");

    // firpfbchr_crcf: M channels, P output decimation. P == M is
    // critically sampled; oversample=2 => P = M/2, each bin at 2x rate.
    const int decim_p = num_channels / oversample;
    if (block_size % decim_p != 0)
        err(fmt("channelizer.block_size (%d) must be a multiple of "
                "num_channels/oversample (%d) so it slices evenly into the "
                "channelizer",
                block_size, decim_p));

    if (proc % num_channels != 0)
        err(fmt("radio.samp_rate %lld is not divisible by "
                "channelizer.num_channels %d -- bin width would be fractional",
                proc, num_channels));
    const long long bin_width = proc / num_channels;

    if ((proc * oversample) % num_channels != 0)
        err(fmt("radio.samp_rate x channelizer.oversample (%lld x %d) is not "
                "divisible by channelizer.num_channels %d -- channelizer output "
                "rate would be fractional",
                proc, oversample, num_channels));
    const long long channel_rate = proc * oversample / num_channels;

    // ---- fm -----------------------------------------------------
    const yaml::Value& fm = cfg.at("fm");
    check_keys(fm, "config.yml fm",
               {"deviation", "tau", "intermediate_rate", "predemod_transition",
                "demod", "declick", "declick_sigma", "pll_bw_hz", "afc",
                "limiter"});
    const int fm_deviation = fm.at("deviation").as_int();
    const double fm_tau = fm.at("tau").as_double();
    const int fm_intermediate_want = fm.at("intermediate_rate").as_int();
    const int predemod_transition = fm.at("predemod_transition").as_int();
    bound(fm_deviation > 0, "fm.deviation", "must be > 0");
    bound(fm_tau > 0.0, "fm.tau", "must be > 0");
    bound(fm_intermediate_want > 0, "fm.intermediate_rate", "must be > 0");
    bound(predemod_transition > 0, "fm.predemod_transition", "must be > 0");

    // Weak-signal demod. All optional; the defaults are open-loop atan2,
    // no declick.
    const std::string fm_demod =
        fm.has("demod") ? fm.at("demod").as_string() : "discriminator";
    bound(fm_demod == "discriminator" || fm_demod == "pll", "fm.demod",
          "must be 'discriminator' or 'pll'");
    const bool fm_declick =
        fm.has("declick") ? fm.at("declick").as_bool() : false;
    const double fm_declick_sigma =
        fm.has("declick_sigma") ? fm.at("declick_sigma").as_double() : 5.0;
    bound(fm_declick_sigma > 1.0, "fm.declick_sigma", "must be > 1.0");
    const double fm_pll_bw_hz =
        fm.has("pll_bw_hz") ? fm.at("pll_bw_hz").as_double() : 0.0;
    bound(fm_pll_bw_hz >= 0.0, "fm.pll_bw_hz", "must be >= 0");
    // AFC carrier-drift tracking. On by default; `fm.afc: false` pins the
    // fine mixer static, for a signal where the discriminator DC mean is
    // too multipath-corrupted for the loop to track cleanly.
    const bool fm_afc = fm.has("afc") ? fm.at("afc").as_bool() : true;
    // Look-ahead limiter; `fm.limiter: false` -> the tanh soft-clip.
    const bool fm_limiter =
        fm.has("limiter") ? fm.at("limiter").as_bool() : true;
    if (fm_demod == "pll" && fm_pll_bw_hz > 0.0)
        bound(2.0 * M_PI * fm_pll_bw_hz / channel_rate <= 0.8, "fm.pll_bw_hz",
              "loop bandwidth too wide for the channel rate (normalised > 0.8)");
    if (fm_demod == "pll")
        cr.findings.push_back(
            "fm.demod: pll is the experimental threshold-extension demod -- "
            "mono stations only; validate with --demod-cal on this DSP "
            "baseline before relying on it");

    // ---- audio ------------------------------------------------
    const yaml::Value& audio = cfg.at("audio");
    check_keys(audio, "config.yml audio", {"rate", "bandwidth", "stop_bandwidth"});
    const int audio_rate = audio.at("rate").as_int();
    const int audio_bw = audio.at("bandwidth").as_int();
    const int audio_stop = audio.at("stop_bandwidth").as_int();
    bound(audio_rate > 0, "audio.rate", "must be > 0");
    bound(audio_bw > 0, "audio.bandwidth", "must be > 0");
    bound(audio_stop > 0, "audio.stop_bandwidth", "must be > 0");
    if (audio_bw >= audio_rate / 2)
        err(fmt("audio.bandwidth %d Hz must be < audio.rate/2 = %d Hz (Nyquist)",
                audio_bw, audio_rate / 2));

    // ---- udp --------------------------------------------------
    const yaml::Value& udp = cfg.at("udp");
    check_keys(udp, "config.yml udp", {"host", "mtu"});
    const std::string udp_host = udp.at("host").as_string();
    const int udp_mtu = udp.at("mtu").as_int();
    bound(udp_mtu > 0 && udp_mtu <= 65507, "udp.mtu", "must be 1-65507");
    if (udp_mtu > 1472)
        cr.findings.push_back(
            fmt("udp.mtu %d is above the typical path MTU (~1472) -- packets "
                "will IP-fragment, and one lost fragment drops a whole audio "
                "datagram",
                udp_mtu));

    // ---- agc / squelch / status ------------------------------
    const detail::AgcCfg g_agc =
        detail::read_agc(cfg.get("agc"), detail::AgcCfg{}, "config.yml agc");
    const detail::SqCfg g_sq =
        detail::read_sq(cfg.get("squelch"), detail::SqCfg{}, "config.yml squelch");
    const detail::StereoCfg g_stereo = detail::read_stereo(
        cfg.get("stereo"), detail::StereoCfg{}, "config.yml stereo");
    const detail::SnrCfg g_snr =
        detail::read_snr(cfg.get("snr"), detail::SnrCfg{}, "config.yml snr");
    {
        const yaml::Value& snr_node = cfg.get("snr");
        if (snr_node.is_scalar() ||
            (snr_node.is_map() && snr_node.has("calibrated")))
            cr.findings.push_back(
                "snr.calibrated / `snr: <bool>` is deprecated and ignored -- "
                "calibrated SNR is always on; drop the "
                "key, keep only snr.k_noise if you set it");
    }
    const detail::ListenerCfg g_listener = detail::read_listener(
        cfg.get("listener"), detail::ListenerCfg{}, "config.yml listener");
    const detail::ScanCfg g_scan =
        detail::read_scan(cfg.get("scan"), detail::ScanCfg{}, "config.yml scan");
    const detail::DecodeCfg g_decode = detail::read_decode(
        cfg.get("decode"), detail::DecodeCfg{}, "config.yml decode");
    const detail::RdsCfg g_rds =
        detail::read_rds(cfg.get("rds"), detail::RdsCfg{}, "config.yml rds");

    int status_port = 0;
    std::string status_host = "127.0.0.1";
    StatusPublic status_public;
    const yaml::Value& st = cfg.get("status");
    if (st.is_scalar()) {
        status_port = st.as_int();
    } else if (st.is_map()) {
        check_keys(st, "config.yml status", {"port", "host", "public"});
        if (st.has("port"))
            status_port = st.at("port").as_int();
        if (st.has("host"))
            status_host = st.at("host").as_string();
        // status.public.* -- which groups of the status JSON the
        // unauthenticated /api/status proxy is allowed to show. Absent ->
        // the StatusPublic defaults (rds_text on, everything else off).
        const yaml::Value& pub = st.get("public");
        if (pub.is_map()) {
            check_keys(pub, "config.yml status.public",
                       {"signal", "stereo", "pilot", "rds_text", "rds_diag",
                        "sdr_health"});
            if (pub.has("signal"))
                status_public.signal = pub.at("signal").as_bool();
            if (pub.has("stereo"))
                status_public.stereo = pub.at("stereo").as_bool();
            if (pub.has("pilot"))
                status_public.pilot = pub.at("pilot").as_bool();
            if (pub.has("rds_text"))
                status_public.rds_text = pub.at("rds_text").as_bool();
            if (pub.has("rds_diag"))
                status_public.rds_diag = pub.at("rds_diag").as_bool();
            if (pub.has("sdr_health"))
                status_public.sdr_health = pub.at("sdr_health").as_bool();
        } else if (!pub.is_null()) {
            err("config.yml status.public: expected a mapping");
        }
    }
    bound(status_port == 0 || (status_port >= 1025 && status_port <= 65535),
          "status.port", "must be 0 (off) or 1025-65535");
    bound(!status_host.empty(), "status.host", "must not be empty");

    // ---- usable channel bandwidth vs pre-demod filter -----------
    const long long carson_bw = 2 * (fm_deviation + audio_bw);
    const long long usable_bw = bin_width * oversample;
    const long long required_bw = carson_bw + 2 * predemod_transition;
    if (usable_bw < required_bw)
        err(fmt("channelizer usable bandwidth %.1f kHz (bin_width %.1f kHz x "
                "oversample %d) < required %.1f kHz [2x(%.0fk dev + %.0fk audio "
                "+ %.0fk transition)]. Decrease num_channels, increase "
                "oversample, or raise radio.samp_rate",
                usable_bw / 1e3, bin_width / 1e3, oversample, required_bw / 1e3,
                fm_deviation / 1e3, audio_bw / 1e3, predemod_transition / 1e3));
    if (usable_bw < required_bw * 12 / 10)
        cr.findings.push_back(
            fmt("usable channel bandwidth %.1f kHz is <1.2x required %.1f kHz -- "
                "tight guard band, adjacent bleed possible",
                usable_bw / 1e3, required_bw / 1e3));

    // ---- calibrated SNR guard band -----------------------------
    // snr_db is always the calibrated, program-independent figure, and
    // every SNR-driven feature is tuned to that scale. MonoStation
    // estimates the discriminator noise PSD from a
    // ~12 kHz guard band just below the pre-demod passband edge
    // ([fm_deviation + audio_bw - 14k, ... - 2k]); it must clear the 67 kHz
    // SCA at the bottom and stay below ~0.45 channel_rate at the top. If
    // the plan can't fit it there is no proxy to fall back to, so fail here.
    {
        const double predemod_pass =
            static_cast<double>(fm_deviation) + audio_bw;
        const double gb_hi = predemod_pass - 2000.0;
        const double gb_lo = gb_hi - 12000.0;
        if (gb_lo < 68000.0)
            err(fmt("calibrated SNR guard band would start at %.1f kHz, below "
                    "the 68 kHz SCA/RDS clearance floor: fm.deviation %.0fk + "
                    "audio.bandwidth %.0fk is too narrow. Raise fm.deviation.",
                    gb_lo / 1e3, fm_deviation / 1e3, audio_bw / 1e3));
        if (gb_hi >= 0.45 * channel_rate)
            err(fmt("calibrated SNR guard band would end at %.1f kHz, above "
                    "0.45 x channel_rate (%.1f kHz): the channelizer plan is "
                    "too narrow. Raise channelizer.oversample or lower "
                    "channelizer.num_channels.",
                    gb_hi / 1e3, 0.45 * channel_rate / 1e3));
    }

    // ---- FM intermediate rate ----------------------------------
    // discriminator / de-emphasis run at channel_rate, then stage A
    // decimates channel_rate -> fm_intermediate.
    const int fm_decim = std::max<long long>(1, channel_rate / fm_intermediate_want);
    const int fm_intermediate = static_cast<int>(channel_rate / fm_decim);
    if (fm_intermediate != fm_intermediate_want)
        err(fmt("fm.intermediate_rate %d Hz is not reachable from channel rate "
                "%lld Hz by integer decimation. Closest: %d Hz (decim %d). Set "
                "fm.intermediate_rate: %d",
                fm_intermediate_want, channel_rate, fm_intermediate, fm_decim,
                fm_intermediate));
    if (fm_intermediate < audio_bw * 2)
        err(fmt("fm.intermediate_rate %d Hz < 2 x audio.bandwidth %d Hz -- audio "
                "would be filtered out before resampling",
                fm_intermediate, audio_bw));

    // ---- audio resampler (integer decimation only) ------------
    if (fm_intermediate % audio_rate != 0)
        err(fmt("audio.rate %d Hz is not an integer division of "
                "fm.intermediate_rate %d Hz. The C++ receiver only does integer "
                "audio decimation -- pick an audio.rate that divides %d",
                audio_rate, fm_intermediate, fm_intermediate));
    const int audio_decim = fm_intermediate / audio_rate;

    // ---- de-emphasis single-pole IIR -------------------------
    // Standard bilinear-transform derivation of the 1/(1 + jw*tau)
    // de-emphasis pole. Runs at channel_rate.
    const double deemph_rate = static_cast<double>(channel_rate);
    const double w_c = 1.0 / fm_tau;
    const double w_ca = 2.0 * deemph_rate * std::tan(w_c / (2.0 * deemph_rate));
    const double k = -w_ca / (2.0 * deemph_rate);
    const double deemph_p1 = (1.0 + k) / (1.0 - k);
    const double deemph_b0 = -k / (1.0 - k);

    // ---- fill Plan (everything but stations) -----------------
    p.sdr_uri = "ip:" + sdr_ip;
    p.sdr_gain_db = gain_db;
    p.sdr_gain_mode = sdr_gain_mode;
    p.sdr_agc_target_dbfs = agc_target_dbfs;
    p.sdr_agc_max_dbfs = agc_max_dbfs;
    p.sdr_agc_min_gain_db = agc_min_gain_db;
    p.sdr_agc_step_db = agc_step_db;
    p.sdr_agc_interval_ms = agc_interval_ms;
    p.sdr_agc_hysteresis_db = agc_hysteresis_db;
    p.sdr_agc_clip_ppm_trip = agc_clip_ppm_trip;
    p.sdr_rx_lo_hz = center_freq;
    p.sdr_hw_rate_hz = samp_rate;
    p.sdr_rf_bandwidth_hz = samp_rate; // let the driver clamp
    p.num_channels = num_channels;
    p.decim_p = decim_p;
    p.oversample = oversample;
    p.proto_semilen_m = proto_semilen_m;
    p.atten_db = atten_db;
    p.channel_rate_hz = static_cast<int>(channel_rate);
    p.proc_rate_hz = proc;
    p.samples_per_block = block_size;
    p.udp_host = udp_host;
    p.udp_mtu = udp_mtu;
    p.audio_rate_hz = audio_rate;
    p.listener_codec = g_listener.codec;
    p.listener_aac_bitrate_mono = g_listener.aac_bitrate_mono;
    p.listener_aac_bitrate_stereo = g_listener.aac_bitrate_stereo;
    p.status_port = status_port;
    p.status_host = status_host;
    p.status_public = status_public;
    p.scan_enabled = g_scan.enabled;
    p.scan_nfft = g_scan.nfft;
    p.scan_average_ms = g_scan.average_ms;
    p.scan_threshold_db = g_scan.threshold_db;
    p.decode_listener_gated = g_decode.listener_gated;

    cr.bin_width = bin_width;
    cr.carson_bw = carson_bw;
    cr.predemod_transition = predemod_transition;
    cr.fm_deviation = fm_deviation;
    cr.fm_tau = fm_tau;
    cr.fm_decim = fm_decim;
    cr.fm_intermediate = fm_intermediate;
    cr.audio_bw = audio_bw;
    cr.audio_stop = audio_stop;
    cr.deemph_b0 = deemph_b0;
    cr.deemph_p1 = deemph_p1;
    cr.audio_decim = audio_decim;
    cr.fm_demod = fm_demod;
    cr.fm_declick = fm_declick;
    cr.fm_declick_sigma = fm_declick_sigma;
    cr.fm_pll_bw_hz = fm_pll_bw_hz;
    cr.fm_afc = fm_afc;
    cr.fm_limiter = fm_limiter;
    cr.agc = g_agc;
    cr.sq = g_sq;
    cr.stereo = g_stereo;
    cr.snr = g_snr;
    cr.rds = g_rds;
    return cr;
}

// The stations.yml half of resolve_text(): raw parse, the enabled-station
// filter, port assignment/uniqueness
// checks, the in-band filter, and per-station bin assignment. Fills in
// `cr.plan.stations`/`cr.plan.all_stations` and appends any findings; `sy`
// is already parsed and confirmed to be a mapping with a `stations` key
// (resolve_text() does that much itself, same shared shape-checking split
// as resolve_config()).
inline void resolve_stations(const yaml::Value& sy, ConfigResolved& cr) {
    using detail::bound;
    using detail::check_keys;
    using detail::err;
    using detail::fmt;

    Plan& p = cr.plan;
    const long long center_freq = p.sdr_rx_lo_hz;
    const long long samp_rate = p.sdr_hw_rate_hz;
    const int num_channels = p.num_channels;
    const int oversample = p.oversample;
    const long long channel_rate = p.channel_rate_hz;
    const int audio_rate = p.audio_rate_hz;
    const long long bin_width = cr.bin_width;
    const int fm_deviation = cr.fm_deviation;
    const double fm_tau = cr.fm_tau;
    const int fm_decim = cr.fm_decim;
    const int fm_intermediate = cr.fm_intermediate;
    const int predemod_transition = cr.predemod_transition;
    const int audio_bw = cr.audio_bw;
    const int audio_stop = cr.audio_stop;
    const double deemph_b0 = cr.deemph_b0;
    const double deemph_p1 = cr.deemph_p1;
    const int audio_decim = cr.audio_decim;
    const detail::AgcCfg& g_agc = cr.agc;
    const detail::SqCfg& g_sq = cr.sq;
    const detail::StereoCfg& g_stereo = cr.stereo;
    const detail::SnrCfg& g_snr = cr.snr;
    const detail::RdsCfg& g_rds = cr.rds;

    check_keys(sy, "stations.yml", {"stations"});
    // `stations:` may be an explicit empty list (or a bare null key) --
    // that's the "receiver up for scan/admin only" mode: SDR + status port
    // + /api/scan + admin write endpoints all come up, just no per-station
    // DSP. The plan (audio_rate etc.) is fully valid; p.stations stays
    // empty and every downstream consumer already tolerates that.
    const yaml::Value& stations_node = sy.at("stations");
    if (!stations_node.is_seq() || stations_node.seq().empty()) {
        cr.findings.push_back(
            "stations.yml: no stations configured -- receiver up for "
            "scan/admin only");
        return;
    }
    const std::vector<yaml::Value>& raw_stations = stations_node.seq();

    struct Raw {
        std::string label;
        long long freq = 0;
        int port = 0;
        bool has_port = false;
        bool enabled = true;
        StereoMode stereo_mode = StereoMode::Mono;
        yaml::Value agc_ov; // per-station override (null / bool / mapping)
        yaml::Value sq_ov;
        yaml::Value rds_ov; // per-station override of the config.yml rds default
    };
    std::vector<Raw> raws;
    std::set<int> seen_ports;
    for (const yaml::Value& s : raw_stations) {
        check_keys(s, "stations.yml station",
                   {"label", "freq", "port", "stereo", "agc", "squelch",
                    "enabled", "rds"});
        Raw r;
        r.label = s.at("label").as_string();
        r.freq = s.at("freq").as_i64();
        if (s.has("port")) {
            r.port = s.at("port").as_int();
            r.has_port = true;
        }
        if (s.has("enabled"))
            r.enabled = s.at("enabled").as_bool();
        // config.yml `stereo.mode` is the default; a per-station `stereo:`
        // overrides it.
        r.stereo_mode = g_stereo.mode;
        if (s.has("stereo"))
            r.stereo_mode = detail::parse_stereo_mode(s.at("stereo"));
        r.agc_ov = s.get("agc");
        r.sq_ov = s.get("squelch");
        r.rds_ov = s.get("rds");

        // label is display-only -- everything downstream (UDP routing,
        // webui) keys off `port`, so the same station may legitimately be
        // listed more than once (a second transmitter/frequency, or a
        // deliberate strongest-signal candidate set).
        if (r.has_port) {
            bound(r.port >= 1025 && r.port <= 65535,
                  "station '" + r.label + "' port",
                  fmt("must be 1025-65535, got %d", r.port));
            if (!seen_ports.insert(r.port).second)
                err(fmt("stations.yml: duplicate station port %d", r.port));
        }
        if (!r.has_port)
            err("station '" + r.label +
                "': missing required 'port:' -- every station must specify "
                "its UDP port explicitly (no more auto-assignment)");
        if (r.freq < 50000000 || r.freq > 1500000000)
            err(fmt("station '%s': freq %.3f MHz is outside the plausible RF "
                    "range 50-1500 MHz",
                    r.label.c_str(), r.freq / 1e6));
        raws.push_back(std::move(r));
    }

    // Captured before the enabled-only filter below so GET /api/scan can
    // still recognize a disabled station's frequency as configured.
    for (const auto& r : raws)
        p.all_stations.push_back({r.label, r.freq});

    // `enabled: false` (default true): drop the station here, before port
    // assignment / bin planning, so it never gets a channelizer bin, a
    // MonoStation, or a UDP port, and is absent from --check --json and
    // /api/stations/active -- same as if it weren't in the file, except
    // the label/freq/port stay around to flip back on later. The port
    // uniqueness check above already ran against the full list (including
    // disabled entries) so re-enabling one can't silently collide.
    {
        std::vector<Raw> enabled_raws;
        for (auto& r : raws)
            if (r.enabled)
                enabled_raws.push_back(std::move(r));
        raws = std::move(enabled_raws);
    }
    if (raws.empty()) {
        cr.findings.push_back(
            "stations.yml: every station is disabled -- receiver up for "
            "scan/admin only");
        return;
    }

    // ---- drop stations outside the receiver's current span --------
    const long long freq_min = center_freq - samp_rate / 2;
    const long long freq_max = center_freq + samp_rate / 2;
    std::vector<Raw> in_band;
    for (const Raw& r : raws) {
        if (r.freq >= freq_min && r.freq <= freq_max) {
            in_band.push_back(r);
        } else {
            cr.findings.push_back(
                fmt("'%s' at %.3f MHz is outside receiver span %.3f-%.3f MHz -- "
                    "ignoring",
                    r.label.c_str(), r.freq / 1e6, freq_min / 1e6,
                    freq_max / 1e6));
        }
    }
    if (in_band.empty()) {
        cr.findings.push_back(
            fmt("stations.yml: no configured station is in the receiver span "
                "%.3f-%.3f MHz -- receiver up for scan/admin only",
                freq_min / 1e6, freq_max / 1e6));
        return;
    }

    // ---- per-station bin assignment + checks --------------------
    // What matters when a carrier sits off its bin centre: its far FM
    // sideband reaches |fine_hz| + carson_half, where carson_half is
    // Carson on the highest modulating frequency the station actually
    // carries -- audio_bw for mono, the 53 kHz stereo L-R subcarrier,
    // ~59.4 kHz once RDS is on -- not just audio.bandwidth. That reach
    // must clear channel Nyquist (channel_rate/2 = bin_width*oversample/2)
    // with a guard for the prototype's transition band; past Nyquist the
    // sidebands alias. This tracks oversample, unlike a fixed fraction of
    // bin_width -- at oversample 2 a carrier can sit at the bin edge and
    // still be clean, at oversample 1 it cannot.
    //
    // Two carriers in one bin are only a decode problem when the
    // per-station predemod filter cannot separate them: a synthetic sweep
    // through the real predemod + discriminator put the "neighbour stops
    // mattering" separation near carson_half + fm_deviation (~160 kHz
    // mono); below carson_half they are co-channel (FM capture, only the
    // stronger decodes). (docs: bin-plan investigation, 2026-09-07.)
    const double rx_center = static_cast<double>(center_freq);
    const int n = num_channels;
    const double channel_half_bw = channel_rate / 2.0;

    struct UsedBin {
        int bin;
        std::string label;
        long long freq;
    };
    std::vector<UsedBin> used_bins;

    for (const Raw& r : in_band) {
        int bin_idx = 0;
        long long k_wrap = 0;
        detail::nearest_wrapped_bin(static_cast<double>(r.freq) - rx_center,
                                    static_cast<double>(bin_width), n, bin_idx,
                                    k_wrap);
        const double bin_center =
            rx_center + static_cast<double>(k_wrap) * bin_width;
        const double fine_hz = static_cast<double>(r.freq) - bin_center;

        const detail::AgcCfg a = detail::read_agc(
            r.agc_ov, g_agc, "stations.yml '" + r.label + "' agc");
        const detail::SqCfg q = detail::read_sq(
            r.sq_ov, g_sq, "stations.yml '" + r.label + "' squelch");
        const detail::RdsCfg rds = detail::read_rds(
            r.rds_ov, g_rds, "stations.yml '" + r.label + "' rds");

        double f_m = static_cast<double>(audio_bw);
        if (r.stereo_mode != StereoMode::Mono)
            f_m = std::max(f_m, 53000.0);
        if (rds.enabled)
            f_m = std::max(f_m, 59400.0);
        const double carson_half = fm_deviation + f_m;
        const double neighbour_clear = carson_half + fm_deviation;

        // ---- sideband reach vs channel Nyquist ----
        const double reach = std::fabs(fine_hz) + carson_half;
        if (reach > channel_half_bw)
            err(fmt("'%s': %+.1f kHz off bin %d centre, the FM signal reaches "
                    "%.1f kHz -- past channel Nyquist %.1f kHz (bin_width %.0f "
                    "kHz x oversample %d / 2); sidebands alias. Retune "
                    "radio.center_freq, raise channelizer.oversample, or reduce "
                    "fm.deviation",
                    r.label.c_str(), fine_hz / 1e3, bin_idx, reach / 1e3,
                    channel_half_bw / 1e3, bin_width / 1e3, oversample));
        else if (reach > channel_half_bw - predemod_transition)
            cr.findings.push_back(fmt(
                "'%s' at %.2f MHz sits %+.1f kHz off bin %d centre -- FM "
                "sidebands reach %.1f kHz, inside the %.0f kHz transition guard "
                "below channel Nyquist %.1f kHz; some HF / RDS roll-off likely",
                r.label.c_str(), r.freq / 1e6, fine_hz / 1e3, bin_idx,
                reach / 1e3, predemod_transition / 1e3, channel_half_bw / 1e3));

        // ---- shared bin, classified by carrier separation ----
        for (const auto& ub : used_bins) {
            if (ub.bin != bin_idx)
                continue;
            const double df = std::fabs(static_cast<double>(r.freq) -
                                        static_cast<double>(ub.freq));
            if (df < carson_half)
                cr.findings.push_back(fmt(
                    "'%s' and '%s' are co-channel in bin %d -- %.0f kHz apart, "
                    "inside the FM signal (half-BW %.0f kHz); FM capture, only "
                    "the stronger carrier decodes",
                    r.label.c_str(), ub.label.c_str(), bin_idx, df / 1e3,
                    carson_half / 1e3));
            else if (df < neighbour_clear)
                cr.findings.push_back(fmt(
                    "'%s' and '%s' share bin %d and are only %.0f kHz apart -- "
                    "the predemod filter's skirt only partly rejects the "
                    "neighbour; the weaker degrades when the two are close in "
                    "level",
                    r.label.c_str(), ub.label.c_str(), bin_idx, df / 1e3));
            else
                cr.findings.push_back(fmt(
                    "'%s' and '%s' share bin %d (%.0f kHz apart) -- one %.0f kHz "
                    "slice feeds both, but the per-station predemod filter "
                    "separates them; no decode penalty",
                    r.label.c_str(), ub.label.c_str(), bin_idx, df / 1e3,
                    bin_width / 1e3));
            break;
        }
        used_bins.push_back({bin_idx, r.label, r.freq});

        // No port-range finding here: `port` is always explicit now (with
        // gaps, or a reserved `enabled: false` entry, deliberately). The
        // invariants that matter (1025-65535, uniqueness) are hard errors
        // during the raw parse above.

        if (r.stereo_mode != StereoMode::Mono && channel_rate < 120000)
            err(fmt("'%s': stereo decode needs a channel rate >= 120 kHz for "
                    "the 19 kHz pilot + 23-53 kHz L-R subcarrier; this config "
                    "yields %lld kHz. Raise channelizer.oversample or "
                    "radio.samp_rate, or set stereo: false",
                    r.label.c_str(), channel_rate / 1000));

        StationPlan sp;
        sp.label = r.label;
        sp.freq_hz = r.freq;
        sp.port = r.port;
        sp.stereo_mode = r.stereo_mode;
        sp.stereo = (r.stereo_mode != StereoMode::Mono);
        sp.stereo_blend = (r.stereo_mode == StereoMode::Auto);
        sp.stereo_pilot_threshold_db = g_stereo.pilot_threshold_db;
        sp.stereo_blend_snr_lo_db = g_stereo.blend_snr_lo_db;
        sp.stereo_blend_snr_hi_db = g_stereo.blend_snr_hi_db;
        sp.stereo_pilot_pll = g_stereo.pilot_pll;
        sp.rds_enabled = rds.enabled;
        sp.bin = bin_idx;
        sp.fine_hz = fine_hz;
        sp.channel_rate_hz = static_cast<int>(channel_rate);
        sp.rf_occupied_bw_hz = static_cast<int>(2.0 * carson_half);
        sp.agc_enabled = a.enabled;
        sp.agc_target = a.target;
        sp.agc_max_gain_db = a.max_gain_db;
        sp.agc_response_ms = a.response_ms;
        sp.squelch_enabled = q.enabled;
        sp.squelch_open_snr_db = q.open_snr_db;
        sp.squelch_hang_ms = q.hang_ms;
        sp.snr_knoise = g_snr.k_noise;
        sp.fm_deviation_hz = fm_deviation;
        sp.fm_tau_s = fm_tau;
        sp.fm_decim = fm_decim;
        sp.fm_intermediate_rate_hz = fm_intermediate;
        sp.fm_predemod_transition_hz = predemod_transition;
        sp.fm_audio_bw_hz = audio_bw;
        sp.fm_audio_stop_hz = audio_stop;
        sp.deemph_b0 = deemph_b0;
        sp.deemph_p1 = deemph_p1;
        sp.audio_decim = audio_decim;
        sp.audio_rate_hz = audio_rate;

        // Weak-signal demod. The PLL demod is mono-only -- its ~45 kHz
        // loop mangles the 23-53 kHz L-R
        // subcarrier and 57 kHz RDS -- so a stereo/rds station asking for
        // it is forced back to the discriminator with a finding.
        sp.fm_declick = cr.fm_declick;
        sp.fm_declick_sigma = cr.fm_declick_sigma;
        sp.fm_pll_bw_hz = cr.fm_pll_bw_hz;
        sp.fm_afc = cr.fm_afc;
        sp.fm_limiter = cr.fm_limiter;
        sp.fm_demod = cr.fm_demod;
        if (sp.fm_demod == "pll" && (sp.stereo || sp.rds_enabled)) {
            cr.findings.push_back(fmt(
                "'%s': fm.demod: pll is mono-only (stereo/RDS subcarriers "
                "need the full-composite discriminator) -- using "
                "'discriminator' for this station",
                r.label.c_str()));
            sp.fm_demod = "discriminator";
        }
        p.stations.push_back(std::move(sp));
    }
}

// Resolve from in-memory YAML text (config first, stations second) --
// glue between resolve_config()/resolve_stations(). The path-based
// resolve() below is a thin wrapper that reads the
// two files and calls this. Draft-validation over HTTP (admin_routes.hpp)
// calls it directly -- no tempdir, no subprocess.
inline ResolveResult resolve_text(const std::string& config_text,
                                  const std::string& stations_text) {
    yaml::Value cfg;
    try {
        cfg = yaml::Value::parse(config_text);
    } catch (const std::exception& e) {
        detail::err(std::string("config.yml: ") + e.what());
    }
    if (!cfg.is_map())
        detail::err("config.yml: top level must be a mapping");
    ConfigResolved cr = resolve_config(cfg);

    yaml::Value sy;
    try {
        sy = yaml::Value::parse(stations_text);
    } catch (const std::exception& e) {
        detail::err(std::string("stations.yml: ") + e.what());
    }
    if (!sy.is_map() || !sy.has("stations"))
        detail::err("stations.yml: expected a top-level 'stations:' list");
    resolve_stations(sy, cr);

    ResolveResult out;
    out.plan = std::move(cr.plan);
    out.findings = std::move(cr.findings);
    return out;
}

// Read config.yml + stations.yml from disk and resolve them. A missing or
// unreadable file is a hard error, same class as a parse failure.
inline ResolveResult resolve(const std::string& config_path,
                             const std::string& stations_path) {
    return resolve_text(detail::read_file(config_path),
                        detail::read_file(stations_path));
}

} // namespace fmrx

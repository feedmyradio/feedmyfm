// Resolved, execution-ready view of config.yml + stations.yml.
//
// Everything here is a plain number: all the frequency-planning math,
// decimation-factor derivation, bin assignment and validation happens
// once in resolve.hpp and is baked into these structs. The DSP code
// (channelizer.hpp, mono_station.hpp, main.cpp) does no planning.
#pragma once

#include <string>
#include <vector>

namespace fmrx {

// How a station's audio is decoded. `Mono` is a 2-byte s16le stream;
// `Stereo` and `Auto` both emit an interleaved L/R (4-byte) stream so the
// on-wire frame format never changes at runtime -- `Auto` just blends L/R
// back toward mono when the pilot is absent or the SNR is poor.
enum class StereoMode { Mono, Stereo, Auto };

// JSON-value spelling of StereoMode -- shared by every station JSON
// emitter (admin_routes.hpp's active_plan_json, main.cpp's
// print_check_json) that echoes stereo_mode as a string. Not for human
// log lines (main.cpp's startup banner uses "mono"/"stereo"/"auto", a
// different convention -- deliberately not merged with this one).
inline const char* stereo_mode_str(StereoMode m) {
    switch (m) {
    case StereoMode::Auto: return "auto";
    case StereoMode::Stereo: return "on";
    default: return "off";
    }
}

struct StationPlan {
    std::string label;
    long long freq_hz = 0;
    int port = 0;
    StereoMode stereo_mode = StereoMode::Mono;
    bool stereo = false; // == (stereo_mode != Mono): emits 4-byte L/R frames

    int bin = 0;          // channelizer output port (FFT-bin order, 0 == DC)
    double fine_hz = 0.0; // fine mixer shift to bring station to DC

    int channel_rate_hz = 0; // channelizer output rate for this station
    // RF bandwidth this station's FM signal actually occupies: Carson on
    // the *highest modulating frequency present*, which is the composite
    // top -- audio_bw for mono, ~53 kHz for a stereo L-R subcarrier,
    // ~59.4 kHz once RDS is on -- not just audio.bandwidth. Drives the
    // channelizer bin-edge / adjacent findings in resolve.hpp.
    int rf_occupied_bw_hz = 0;

    int fm_deviation_hz = 0;
    double fm_tau_s = 0.0;
    int fm_decim = 1;
    int fm_intermediate_rate_hz = 0;
    int fm_predemod_transition_hz = 0;
    int fm_audio_bw_hz = 0;
    int fm_audio_stop_hz = 0;

    double deemph_b0 = 0.0;
    double deemph_p1 = 0.0;

    // Weak-signal demod (config `fm.demod` / `fm.declick*` / `fm.pll_bw_hz`).
    // Global fm.* values, no per-station knob.
    //   fm_demod          -- "discriminator" (default) | "pll" (mono only)
    //   fm_declick        -- impulsive-click suppressor on the discriminator
    //   fm_declick_sigma  -- robust-deviation multiplier that marks a click
    //   fm_pll_bw_hz       -- PLL-demod loop bandwidth; <= 0 -> compiled default
    //   fm_afc             -- carrier-drift tracking; false pins the mixer static
    //   fm_limiter         -- look-ahead limiter; false -> tanh soft-clip
    std::string fm_demod = "discriminator";
    bool fm_declick = false;
    double fm_declick_sigma = 5.0;
    double fm_pll_bw_hz = 0.0;
    bool fm_afc = true;
    bool fm_limiter = true;

    int audio_decim = 1; // fm_intermediate_rate -> audio_rate (integer)
    int audio_rate_hz = 0;

    // audio leveling + squelch: resolved from the config `agc:` / `squelch:`
    // blocks with any per-station override merged in.
    bool agc_enabled = true;
    double agc_target = 0.5;      // target output RMS, fraction of full scale
    double agc_max_gain_db = 40.0;
    double agc_response_ms = 200.0;

    bool squelch_enabled = false;
    double squelch_open_snr_db = 20.0; // noise-ratio metric threshold to open
    double squelch_hang_ms = 800.0;

    // status-port snr_db is always the calibrated guard-band figure.
    // Resolved from the config `snr:` block -- system-wide, no
    // per-station override.
    double snr_knoise = 0.0; // config snr.k_noise; <= 0 -> compiled default

    // stereo decode (only meaningful when stereo_mode != Mono). Resolved
    // from the config `stereo:` block; the per-station knob is just the
    // mode (auto / true / false).
    bool stereo_blend = false;              // Auto: crossfade L/R -> mono on poor SNR
    double stereo_pilot_threshold_db = -30.0; // 19 kHz pilot power, dBc of composite,
                                              // above which stereo locks
    // Blend window, read on the calibrated program-independent snr_db scale.
    double stereo_blend_snr_lo_db = 20.0;   // calibrated snr_db: full mono at/below
    double stereo_blend_snr_hi_db = 34.0;   // calibrated snr_db: full stereo at/above
    bool stereo_pilot_pll = true;           // false -> normalise-and-square carrier

    // RDS decoding (Radio Data System). Set by the config.yml `rds:` global
    // default, overridable per station via `rds:` in stations.yml. Requires
    // channel_rate_hz >= 120000 (composite must reach ~59.4 kHz). Under
    // decode.listener_gated it's only decoded for actively-listened stations
    // (set via POST /active); the decoder decimates to a ~160 kHz working
    // rate so the cost is ~+1-2 pp per active station.
    bool rds_enabled = false;
};

// Which groups of the status-port JSON the *public* listener page (webui's
// unauthenticated /api/status proxy) is allowed to see. Config
// `status.public.*`. Everything defaults off except rds_text so the
// now-playing / lock-screen text keeps working; the authenticated
// GET /api/status/full always runs with `all()`. (`--check --json` is a
// separate, StatusPublic-independent JSON shape -- see print_check_json
// in main.cpp.)
struct StatusPublic {
    bool signal = false;     // per-station rf_dbfs / snr_db / squelch / agc /
                             // afc / high_cut / multipath / demod / declick
    bool stereo = false;     // per-station stereo_frac -- the live stereo /
                             // auto-blend state (drives the STEREO badge)
    bool pilot = false;      // per-station pilot_db / pilot_lock / pilot_hz --
                             // the 19 kHz pilot detail behind the stereo state
    bool rds_text = true;    // rds.lock / ps / rt / ptyn / pty / pty_name /
                             // tp / ta
    bool rds_diag = false;   // rds.pi / ber / groups_per_sec / groups_ok / ct
    bool sdr_health = false; // the whole `sdr` object (the `decode` object is
                             // always emitted -- webui's relay needs it)

    static StatusPublic all() { return {true, true, true, true, true, true}; }
};

struct Plan {
    // sdr
    std::string sdr_uri;
    int sdr_gain_db = 0;
    long long sdr_rx_lo_hz = 0;
    long long sdr_hw_rate_hz = 0;
    long long sdr_rf_bandwidth_hz = 0;

    // Front-end gain mode (config `sdr.gain_mode`).
    //   manual                     -- fixed hardwaregain = sdr_gain_db (default)
    //   slow_attack/fast_attack/hybrid -- the AD9361's own AGC; sdr_gain_db
    //                                 is the initial hardwaregain, then the
    //                                 chip owns it
    //   auto_sw                    -- chip stays in manual, the software
    //                                 peak-headroom loop below owns the gain
    std::string sdr_gain_mode = "manual";

    // Software AGC loop (`sdr.agc.*`, only read when sdr_gain_mode == auto_sw).
    // Peak-headroom regulation: the gain only moves when the composite
    // threatens the ADC or the whole band drops. sdr_gain_db is the ceiling
    // + startup + reconnect gain.
    double sdr_agc_target_dbfs = -9.0;   // regulate composite peak EMA to here
    double sdr_agc_max_dbfs = -3.0;      // step down (skipping the interval) above this
    double sdr_agc_min_gain_db = 0.0;    // lower clamp; upper clamp is sdr_gain_db
    double sdr_agc_step_db = 1.0;        // gain change per adjustment
    int sdr_agc_interval_ms = 2000;      // min spacing between step-ups / soft step-downs
    double sdr_agc_hysteresis_db = 3.0;  // step up only below target - this
    double sdr_agc_clip_ppm_trip = 5.0;  // clipped-sample rate forcing an immediate step down

    // channelizer
    int num_channels = 0;
    int decim_p = 0;
    int oversample = 1;
    int proto_semilen_m = 0;
    int atten_db = 0;
    int channel_rate_hz = 0;
    long long proc_rate_hz = 0;
    int samples_per_block = 0;

    // udp
    std::string udp_host;
    int udp_mtu = 0;

    int audio_rate_hz = 0;

    // listener audio format (config `listener:` block). Validated here but
    // consumed downstream by webui -- the receiver's UDP output is identical
    // for `pcm` and `aac`; rx only echoes this via --check --json.
    std::string listener_codec = "pcm"; // pcm | aac
    int listener_aac_bitrate_mono = 96000;
    int listener_aac_bitrate_stereo = 128000;

    // listener-gated decode (config `decode:` block). When true, the RT
    // loop only runs a station's per-block chain while webui's relay says
    // it has a live listener (pushed via POST /active); idle stations cost
    // ~0. Fail-open: unknown/stale => decode everything. Default false =
    // decode every configured station unconditionally, as before.
    bool decode_listener_gated = false;

    // status / admin HTTP endpoint (config `status:` block).
    // status_port 0 = disabled. status_host defaults to loopback; set it
    // to a LAN address only when webui runs on another machine -- doing so
    // requires FEEDMYFM_RX_ADMIN_PASSWORD to be set (enforced at startup).
    int status_port = 0;
    std::string status_host = "127.0.0.1";
    StatusPublic status_public; // what the unauthenticated status proxy shows

    // passive band scan (config `scan:` block), served on GET /api/scan.
    // The averaged periodogram only runs while a scan request is pending;
    // idle cost is one branch per block. Disabled if scan_enabled is false
    // or status_port is 0.
    bool scan_enabled = true;
    int scan_nfft = 4096;          // FFT size (power of two)
    int scan_average_ms = 700;     // how long to accumulate per scan
    double scan_threshold_db = 8.0; // carrier detection margin over the noise floor

    std::vector<StationPlan> stations;

    // label + freq for every stations.yml entry, enabled or not -- unlike
    // `stations` above (which drops disabled entries entirely), this is
    // what GET /api/scan matches detected carriers against, so a disabled
    // station's frequency still reads as "configured" instead of looking
    // like a brand-new carrier to add.
    struct ConfiguredStation {
        std::string label;
        long long freq_hz = 0;
    };
    std::vector<ConfiguredStation> all_stations;
};

} // namespace fmrx

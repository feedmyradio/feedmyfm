// feedmyfm-rx: standalone mono/stereo FM multi-station receiver.
//
//   feedmyfm-rx -c config.yml -s stations.yml             # live, from the SDR
//   feedmyfm-rx -c config.yml -s stations.yml --selftest  # synthetic, no SDR
//   feedmyfm-rx -c config.yml -s stations.yml --check     # validate and exit
//
// config.yml + stations.yml are parsed and validated in-process (see
// resolve.hpp).
//
// Mono stations run the chain in mono_station.hpp; stations marked
// `stereo: true` / `stereo: auto` add the pilot/subcarrier decode in
// stereo_decoder.hpp (auto blends L/R back to mono on a weak signal).
#include <atomic>
#include <cerrno>
#include <cmath>
#include <complex>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include <ctime>

#include "active_set.hpp"
#include "admin_routes.hpp"
#include "channelizer.hpp"
#include "mono_station.hpp"
#include "plan.hpp"
#include "pluto_source.hpp"
#include "resolve.hpp"
#include "scan.hpp"
#include "selftest.hpp"
#include "spectrum.hpp"
#include "station_pool.hpp"
#include "status_server.hpp"

using fmrx::cfloat;

namespace {

std::atomic<bool> g_stop{false};
std::atomic<bool> g_terminate{false};
void on_signal(int) {
    g_stop.store(true);
    g_terminate.store(true);
}

// --- --watch: re-exec on config change -----------------------------
// feedmyfm-rx has no in-process hot reload -- rebuilding the channelizer,
// every per-station chain and the SDR source safely mid-stream is a lot of
// moving parts. Instead, in --watch mode a background thread polls the two
// config files; when one changes, settles, and still resolves cleanly, it
// replaces this process with a fresh copy of itself (execv). execv does
// not unwind the stack, so no destructor runs -- fds survive into the new
// image unless they are O_CLOEXEC, which every socket we open is (see
// status_server.hpp / udp_sink.hpp), so the kernel drops them on exec and
// the SDR is re-acquired on the way back up. A save costs a ~1-3 s audio
// gap. An edit that does NOT resolve is logged and ignored -- the running
// receiver stays up.
struct FileStamp {
    long long mtime_ns = -1;
    long long size = -1;
    bool operator!=(const FileStamp& o) const {
        return mtime_ns != o.mtime_ns || size != o.size;
    }
};

FileStamp stat_file(const std::string& path) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0)
        return {};
    return {static_cast<long long>(st.st_mtim.tv_sec) * 1000000000LL +
                st.st_mtim.tv_nsec,
            static_cast<long long>(st.st_size)};
}

void start_config_watch(const std::string& config_path,
                        const std::string& stations_path, char** argv) {
    std::thread([config_path, stations_path, argv] {
        FileStamp c = stat_file(config_path), s = stat_file(stations_path);
        while (!g_terminate.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            FileStamp c2 = stat_file(config_path), s2 = stat_file(stations_path);
            if (!(c2 != c) && !(s2 != s))
                continue;
            // let a multi-write save finish, then re-read
            std::this_thread::sleep_for(std::chrono::seconds(1));
            c = stat_file(config_path);
            s = stat_file(stations_path);
            if (g_terminate.load())
                return;
            try {
                fmrx::resolve(config_path, stations_path);
            } catch (const std::exception& e) {
                std::fprintf(stderr,
                             "watch: config changed but does not resolve, "
                             "keeping the running receiver: %s\n",
                             e.what());
                continue;
            }
            std::fprintf(stderr, "watch: config changed and validates -- "
                                 "restarting receiver\n");
            std::fflush(nullptr);
            ::execv("/proc/self/exe", argv);
            std::fprintf(stderr, "watch: execv failed (%s) -- staying up\n",
                         std::strerror(errno));
        }
    }).detach();
}

// --- JSON emitters for --check --json and the status port ------------
// scripts/ shell out to --check --json instead of re-implementing config
// validation (webui now reads the resolved plan over HTTP instead).
// Both route string escaping through fmrx::http_detail::json_quote and
// numeric formatting through fmrx::http_detail::json_num, so a nan/inf
// from a degenerate metric gets the same finite guard here as on the
// status port, instead of emitting a bare `nan`/`inf` token, which isn't
// valid JSON.
void print_check_json(const fmrx::Plan& plan,
                      const std::vector<std::string>& findings) {
    namespace jd = fmrx::http_detail;
    const long long bin_width_hz =
        plan.num_channels ? plan.sdr_hw_rate_hz / plan.num_channels : 0;
    std::string o = "{\"ok\":true,";
    o += "\"audio_rate_hz\":" + std::to_string(plan.audio_rate_hz) + ",";
    o += "\"center_freq_hz\":" + std::to_string(plan.sdr_rx_lo_hz) + ",";
    o += "\"samp_rate_hz\":" + std::to_string(plan.sdr_hw_rate_hz) + ",";
    o += "\"sdr\":{\"gain_db\":" + std::to_string(plan.sdr_gain_db) +
         ",\"gain_mode\":" + jd::json_quote(plan.sdr_gain_mode);
    if (plan.sdr_gain_mode == "auto_sw") {
        o += ",\"agc\":{\"target_dbfs\":" +
             jd::json_num(plan.sdr_agc_target_dbfs, 1) +
             ",\"max_dbfs\":" + jd::json_num(plan.sdr_agc_max_dbfs, 1) +
             ",\"min_gain_db\":" + jd::json_num(plan.sdr_agc_min_gain_db, 1) +
             ",\"step_db\":" + jd::json_num(plan.sdr_agc_step_db, 1) +
             ",\"interval_ms\":" + std::to_string(plan.sdr_agc_interval_ms) +
             ",\"hysteresis_db\":" + jd::json_num(plan.sdr_agc_hysteresis_db, 1) +
             ",\"clip_ppm_trip\":" + jd::json_num(plan.sdr_agc_clip_ppm_trip, 1) +
             "}";
    }
    o += "},";
    o += "\"num_channels\":" + std::to_string(plan.num_channels) + ",";
    o += "\"bin_width_hz\":" + std::to_string(bin_width_hz) + ",";
    o += "\"channel_rate_hz\":" + std::to_string(plan.channel_rate_hz) + ",";
    o += "\"status_port\":" + std::to_string(plan.status_port) + ",";
    {
        const auto& sp = plan.status_public;
        auto b = [](bool v) { return std::string(v ? "true" : "false"); };
        o += "\"status_public\":{\"signal\":" + b(sp.signal) +
             ",\"stereo\":" + b(sp.stereo) +
             ",\"pilot\":" + b(sp.pilot) +
             ",\"rds_text\":" + b(sp.rds_text) +
             ",\"rds_diag\":" + b(sp.rds_diag) +
             ",\"sdr_health\":" + b(sp.sdr_health) + "},";
    }
    o += "\"listener\":{\"codec\":" + jd::json_quote(plan.listener_codec) +
         ",\"aac_bitrate_mono\":" +
         std::to_string(plan.listener_aac_bitrate_mono) +
         ",\"aac_bitrate_stereo\":" +
         std::to_string(plan.listener_aac_bitrate_stereo) + "},";
    o += "\"scan\":{\"enabled\":" +
         std::string(plan.scan_enabled ? "true" : "false") +
         ",\"nfft\":" + std::to_string(plan.scan_nfft) +
         ",\"average_ms\":" + std::to_string(plan.scan_average_ms) +
         ",\"threshold_db\":" + jd::json_num(plan.scan_threshold_db, 1) + "},";
    o += "\"decode\":{\"listener_gated\":" +
         std::string(plan.decode_listener_gated ? "true" : "false") + "},";
    o += "\"findings\":[";
    for (size_t i = 0; i < findings.size(); ++i)
        o += (i ? "," : "") + jd::json_quote(findings[i]);
    o += "],\"stations\":[";
    for (size_t i = 0; i < plan.stations.size(); ++i) {
        const auto& s = plan.stations[i];
        o += (i ? "," : "");
        o += "{\"label\":" + jd::json_quote(s.label);
        o += ",\"freq_hz\":" + std::to_string(s.freq_hz);
        o += ",\"port\":" + std::to_string(s.port);
        o += ",\"stereo\":" + std::string(s.stereo ? "true" : "false");
        o += ",\"stereo_mode\":" +
             jd::json_quote(fmrx::stereo_mode_str(s.stereo_mode));
        o += ",\"bin\":" + std::to_string(s.bin);
        o += ",\"fine_hz\":" + jd::json_num(s.fine_hz, 1);
        o += ",\"rf_occupied_bw_hz\":" + std::to_string(s.rf_occupied_bw_hz);
        o += ",\"agc\":" + std::string(s.agc_enabled ? "true" : "false");
        o += ",\"squelch\":" + std::string(s.squelch_enabled ? "true" : "false");
        o += "}";
    }
    o += "]}";
    std::puts(o.c_str());
}

// One JSON object for the --status-port endpoint: SDR link + per-block
// health, and the live per-station signal metrics each MonoStation
// publishes via atomics.
//
// `pub` (config `status.public.*`) gates whole
// groups of fields out of the frame the unauthenticated /api/status proxy
// serves -- a fixed antenna's per-station RF level / SNR / multipath /
// pilot frequency across the band, plus RDS PI + clock time, is a site
// fingerprint. GET /api/status/full (auth) and --check --json always pass
// StatusPublic::all(). `label` / `port` / `idle` / `stereo` and the
// top-level `ts` are always emitted so the listener page still functions.
std::string status_json(
    const fmrx::Plan& plan,
    const std::vector<std::unique_ptr<fmrx::MonoStation>>& stations,
    const fmrx::PlutoSource& src, unsigned long blocks, double rt_pct,
    bool decode_gated, bool decode_controlled, double sdr_peak_dbfs,
    double sdr_clip_ppm, const fmrx::StatusPublic& pub) {
    namespace jd = fmrx::http_detail;
    auto jbool = [](bool v) { return std::string(v ? "true" : "false"); };
    std::string o = "{";
    o += "\"ts\":" + std::to_string(static_cast<long long>(std::time(nullptr)));
    if (pub.sdr_health) {
        o += ",\"sdr\":{\"link_up\":" + jbool(src.link_up());
        o += ",\"overruns\":" + std::to_string(src.overruns());
        o += ",\"blocks\":" + std::to_string(blocks);
        o += ",\"rt_percent\":" + jd::json_num(rt_pct);
        // Front-end gain. `hardwaregain_db` is
        // polled once per status build -- an attr read over USB/network --
        // and the last good value is reused while the link is momentarily
        // down or the phy channel is contended. `peak_dbfs` / `clip_ppm`
        // are the composite ADC-headroom metrics from the RT loop's
        // raw-block scan.
        o += ",\"gain_mode\":" + jd::json_quote(src.gain_mode());
        {
            static double s_last_hw_gain = std::nan("");
            const double g = src.hardware_gain_db();
            if (std::isfinite(g))
                s_last_hw_gain = g;
            if (std::isfinite(s_last_hw_gain))
                o += ",\"hardwaregain_db\":" + jd::json_num(s_last_hw_gain, 1);
        }
        o += ",\"peak_dbfs\":" + jd::json_num(sdr_peak_dbfs, 1);
        o += ",\"clip_ppm\":" + jd::json_num(sdr_clip_ppm, 1);
        o += "}";
    }
    // Listener-gated decode (config decode.listener_gated). `gated` is the
    // toggle; `controlled` is whether relay's active-set is actually in
    // force right now (false = toggle on but no recent POST => fail-open,
    // decoding everything). Always emitted -- it's a config-mode fact, not
    // a signal measurement, and webui's relay uses it to know whether
    // POST /active is worth sending. webui greys out per-station signal
    // fields for stations reporting "idle": true below.
    o += ",\"decode\":{\"listener_gated\":" + jbool(decode_gated) +
         ",\"controlled\":" + jbool(decode_controlled) + "}";
    o += ",\"stations\":[";
    for (size_t i = 0; i < stations.size(); ++i) {
        const auto& st = *stations[i];
        const auto& sp = plan.stations[i];
        o += (i ? "," : "");
        o += "{\"label\":" + jd::json_quote(sp.label);
        o += ",\"port\":" + std::to_string(sp.port);
        o += ",\"idle\":" + jbool(st.idle());
        o += ",\"stereo\":" + jbool(sp.stereo);
        if (pub.signal) {
            o += ",\"rf_dbfs\":" + jd::json_num(st.rf_dbfs());
            o += ",\"snr_db\":" + jd::json_num(st.snr_db());
            o += ",\"squelch_metric_db\":" +
                 jd::json_num(st.squelch_metric_db());
            o += ",\"squelch_open\":" + jbool(st.squelch_open());
            o += ",\"agc_gain_db\":" + jd::json_num(st.agc_gain_db());
            o += ",\"afc_hz\":" + jd::json_num(st.afc_hz(), 1);
            o += ",\"high_cut_hz\":" + jd::json_num(st.high_cut_hz(), 0);
            o += ",\"multipath\":" + jd::json_num(st.multipath(), 1);
            // Demod in use + click-suppression rate. `declick_ppm` is 0
            // unless fm.declick is on.
            o += ",\"demod\":" + jd::json_quote(st.demod_mode());
            o += ",\"declick_ppm\":" + jd::json_num(st.declick_ppm(), 1);
        }
        // `stereo_frac` is the live stereo / auto-blend state (drives the
        // listener page's STEREO badge); `pilot_*` is the 19 kHz pilot
        // detail underneath it -- gated separately so the badge can show
        // without exposing the pilot RF measurements.
        if (st.stereo() && pub.stereo)
            o += ",\"stereo_frac\":" + jd::json_num(st.stereo_frac());
        if (st.stereo() && pub.pilot) {
            o += ",\"pilot_db\":" + jd::json_num(st.pilot_db());
            // Pilot PLL: a real phase-lock flag and the tracked 19 kHz
            // pilot frequency (cross-checks afc_hz).
            o += ",\"pilot_lock\":" + jbool(st.pilot_lock());
            o += ",\"pilot_hz\":" + jd::json_num(st.pilot_hz(), 1);
        }
        // RDS: present only for stations with `rds: true` that are being
        // decoded right now (in the listener-gated active set), and only
        // for the enabled halves -- `rds_text` (the PS/RT the now-playing
        // line and lock screen use) vs `rds_diag` (PI / block errors /
        // group rate). Absent otherwise -- webui hides the readout when
        // the key is missing.
        if (st.rds() && (pub.rds_text || pub.rds_diag)) {
            const fmrx::RdsPublic r = st.rds_snapshot();
            o += ",\"rds\":{";
            bool rf = true; // first field in the rds object?
            auto rsep = [&] {
                if (rf) {
                    rf = false;
                    return std::string();
                }
                return std::string(",");
            };
            if (pub.rds_text) {
                o += rsep() + "\"lock\":" + jbool(r.lock);
                if (r.pty >= 0) {
                    o += rsep() + "\"pty\":" + std::to_string(r.pty);
                    o += rsep() + "\"pty_name\":" +
                         jd::json_quote(fmrx::rds::pty_name(r.pty));
                }
                o += rsep() + "\"tp\":" + jbool(r.tp);
                o += rsep() + "\"ta\":" + jbool(r.ta);
                if (!r.ps.empty())
                    o += rsep() + "\"ps\":" + jd::json_quote(r.ps);
                if (!r.rt.empty())
                    o += rsep() + "\"rt\":" + jd::json_quote(r.rt);
                if (!r.ptyn.empty())
                    o += rsep() + "\"ptyn\":" + jd::json_quote(r.ptyn);
            }
            if (pub.rds_diag) {
                if (r.pi >= 0)
                    o += rsep() + "\"pi\":" + std::to_string(r.pi);
                if (!r.ct_iso.empty())
                    o += rsep() + "\"ct\":" + jd::json_quote(r.ct_iso);
                o += rsep() + "\"ber\":" + jd::json_num(r.block_error_rate, 3);
                o += rsep() + "\"groups_per_sec\":" +
                     jd::json_num(r.groups_per_sec, 1);
                o += rsep() + "\"groups_ok\":" +
                     std::to_string(static_cast<long long>(r.groups_ok));
            }
            o += "}";
        }
        o += "}";
    }
    o += "]}";
    return o;
}

struct Args {
    std::string config_path = "/config/config.yml";
    std::string stations_path = "/config/stations.yml";
    bool check = false;
    bool json = false;
    bool watch = false;
    bool selftest = false;
    bool stereo_selftest = false;
    bool sweep = false;
    bool snr_cal = false;
    bool demod_cal = false;
    bool scan = false;
    double selftest_seconds = 2.0;
    bool verbose = false;
    int proto_m = 0; // >0 overrides channelizer.proto_semilen_m (perf experiments)
    int threads = 0; // >0 forces the station worker-pool size

    // --optimize-cf: grid-search radio.center_freq for the best worst-case
    // per-station distance from a channelizer bin edge.
    bool optimize_cf = false;
    long long opt_step_hz = 1000;
    long long opt_range_hz = -1; // <0 -> one bin width either side
    int opt_top = 10;
    bool opt_allow_count_change = false;
};

Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", what);
                std::exit(2);
            }
            return argv[++i];
        };
        if (s == "--config" || s == "-c")
            a.config_path = next("--config");
        else if (s == "--stations" || s == "-s")
            a.stations_path = next("--stations");
        else if (s == "--check")
            a.check = true;
        else if (s == "--json")
            a.json = true;
        else if (s == "--watch")
            a.watch = true;
        else if (s == "--selftest")
            a.selftest = true;
        else if (s == "--stereo-selftest")
            a.stereo_selftest = true;
        else if (s == "--sweep")
            a.sweep = true;
        else if (s == "--snr-cal")
            a.snr_cal = true;
        else if (s == "--demod-cal")
            a.demod_cal = true;
        else if (s == "--scan")
            a.scan = true;
        else if (s == "--optimize-cf")
            a.optimize_cf = true;
        else if (s == "--step-hz")
            a.opt_step_hz = std::stoll(next("--step-hz"));
        else if (s == "--range-hz")
            a.opt_range_hz = std::stoll(next("--range-hz"));
        else if (s == "--top")
            a.opt_top = std::stoi(next("--top"));
        else if (s == "--allow-station-count-change")
            a.opt_allow_count_change = true;
        else if (s == "--seconds")
            a.selftest_seconds = std::stod(next("--seconds"));
        else if (s == "--verbose" || s == "-v")
            a.verbose = true;
        else if (s == "--proto-m")
            a.proto_m = std::stoi(next("--proto-m"));
        else if (s == "--threads")
            a.threads = std::stoi(next("--threads"));
        else if (s == "--help" || s == "-h") {
            std::puts(
                "usage: feedmyfm-rx [-c config.yml] [-s stations.yml] "
                "[--check [--json] | --selftest | --stereo-selftest | --sweep | "
                "--snr-cal | --demod-cal | --scan [--json] | --optimize-cf] "
                "[--watch] [--seconds N] [--proto-m M] [--threads N] "
                "[--verbose]\n"
                "  --optimize-cf [--step-hz N] [--range-hz N] [--top N] "
                "[--allow-station-count-change]");
            std::exit(0);
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", s.c_str());
            std::exit(2);
        }
    }
    return a;
}

// Synthetic (no-SDR) test/measurement modes -- --selftest,
// --stereo-selftest, --sweep, --snr-cal, --demod-cal, --scan -- live in
// selftest.hpp: first_mono_station/first_stereo_station,
// Tone/synth_wideband/synth_stereo_wideband, tone_amplitude, SynthResult/
// process_synthetic, deemph_db/output_audio_snr, run_sweep/run_snr_cal/
// run_demod_cal/run_scan/run_selftest/run_stereo_selftest. All in fmrx::;
// called (only from main() below) as fmrx::run_sweep(...) etc.

int run_live(const fmrx::Plan& plan, bool verbose, int want_threads,
             const std::string& config_path, const std::string& stations_path) {
    std::printf("feedmyfm-rx: %zu station(s), SDR %s @ %.3f MHz, "
                "%lld Sps, block %d\n",
                plan.stations.size(), plan.sdr_uri.c_str(),
                plan.sdr_rx_lo_hz / 1e6, plan.sdr_hw_rate_hz,
                plan.samples_per_block);

    fmrx::Channelizer chan(plan.num_channels, plan.decim_p,
                           plan.proto_semilen_m, plan.atten_db);
    {
        std::vector<int> active;
        for (const auto& s : plan.stations)
            active.push_back(s.bin);
        chan.set_active_bins(std::move(active));
    }

    std::vector<std::unique_ptr<fmrx::MonoStation>> stations;
    for (const auto& s : plan.stations) {
        stations.push_back(std::make_unique<fmrx::MonoStation>(s, plan.udp_host,
                                                              plan.udp_mtu));
        const char* mode = s.stereo_mode == fmrx::StereoMode::Auto ? "auto"
                           : s.stereo_mode == fmrx::StereoMode::Stereo
                               ? "stereo"
                               : "mono";
        std::printf("  station: %-24s  %.3f MHz  bin %-3d  fine %+8.1f Hz  "
                    "%-6s -> udp %s:%d\n",
                    s.label.c_str(), s.freq_hz / 1e6, s.bin, s.fine_hz, mode,
                    plan.udp_host.c_str(), s.port);
    }

    fmrx::PlutoSource src(plan.sdr_uri, plan.sdr_rx_lo_hz, plan.sdr_hw_rate_hz,
                          plan.sdr_rf_bandwidth_hz, plan.sdr_gain_db,
                          static_cast<size_t>(plan.samples_per_block),
                          plan.sdr_gain_mode);

    // Per-station chains are independent -- run them across cores.
    const int nsta = static_cast<int>(stations.size());

    // --- listener-gated decode (config decode.listener_gated) ---------
    // webui's relay POSTs the set of stations with a live listener to
    // POST /active; `active` holds it (fail-open: all-on until the first
    // POST, and again if the relay goes quiet). Each block the RT loop
    // refreshes `station_live` from it and, on a change, narrows the
    // channelizer copy-out to the live bins. `gate` false => this whole
    // path is inert and every station decodes unconditionally, as before.
    const bool gate = plan.decode_listener_gated &&
                      nsta <= fmrx::ActiveSet::kMaxStations;
    if (plan.decode_listener_gated && !gate)
        std::fprintf(stderr,
                     "  listener-gated decode: DISABLED -- %d stations "
                     "exceeds the %d-station limit; decoding all\n",
                     nsta, fmrx::ActiveSet::kMaxStations);
    fmrx::ActiveSet active(nsta);
    std::vector<unsigned char> station_live(static_cast<size_t>(nsta), 1);

    fmrx::StationPool pool(
        nsta,
        [&](int k) {
            if (gate && !station_live[static_cast<size_t>(k)]) {
                stations[k]->mark_idle();
                return;
            }
            const auto& b = chan.bin(plan.stations[k].bin);
            stations[k]->process(b.data(), b.size());
        },
        want_threads);
    std::fprintf(stderr, "  station pool: %d worker thread(s)\n",
                 pool.thread_count());
    if (gate)
        std::fprintf(stderr,
                     "  listener-gated decode: ON (POST /active; fail-open "
                     "after %lld ms of silence)\n",
                     static_cast<long long>(fmrx::ActiveSet::kFailOpenMs));

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    const bool timing = std::getenv("FMRX_TIMING") != nullptr;
    const double block_seconds =
        static_cast<double>(plan.samples_per_block) / plan.sdr_hw_rate_hz;

    // --- passive band scan (GET /api/scan) ---------------------------
    // The periodogram is fed by the RT loop only while a scan request is
    // pending. The HTTP handler bumps `scan_req`; the RT loop resets the
    // Spectrum, feeds it for `scan_feed_blocks` blocks, publishes the PSD
    // under `scan_mtx`, and bumps `scan_done`. Idle cost: one atomic load
    // + branch per block.
    std::unique_ptr<fmrx::Spectrum> spectrum;
    if (plan.scan_enabled && plan.status_port > 0)
        spectrum = std::make_unique<fmrx::Spectrum>(
            static_cast<unsigned>(plan.scan_nfft),
            static_cast<double>(plan.proc_rate_hz), plan.sdr_rx_lo_hz);
    std::mutex scan_mtx;
    std::vector<float> scan_psd; // published PSD, guarded by scan_mtx
    std::vector<float> scan_scratch;
    std::atomic<std::uint64_t> scan_req{0};  // HTTP -> RT: request a scan
    std::atomic<std::uint64_t> scan_done{0}; // RT -> HTTP: last completed req
    std::uint64_t scan_req_seen = 0;
    int scan_feed_left = 0;
    long long scan_feed_blocks = 0;
    if (spectrum) {
        scan_feed_blocks = static_cast<long long>(
            std::ceil((plan.scan_average_ms / 1000.0) / block_seconds));
        // ensure at least a couple of full transforms even for a short
        // window against a large block size
        const long long min_blocks =
            (2LL * plan.scan_nfft + plan.samples_per_block - 1) /
            plan.samples_per_block;
        scan_feed_blocks = std::max<long long>(
            1, std::max(scan_feed_blocks, min_blocks));
    }

    double t_chan = 0.0, t_sta = 0.0;
    unsigned long blocks = 0;
    std::atomic<double> rt_ema{0.0}; // EMA of per-block busy %, for status
    bool last_link = true;

    // Composite front-end headroom. The RT loop scans each raw block for
    // peak |re|,|im| and a hard-clip count,
    // EMA-smooths both, and publishes them here for the status port and the
    // software AGC loop. Independent of whether gain_mode is auto_sw.
    std::atomic<double> sdr_peak_dbfs{-120.0};
    std::atomic<double> sdr_clip_ppm{0.0};

    // Last listener-gated mask the RT loop acted on. A sentinel that no
    // real effective() value can equal (bit 63 is never a valid station
    // index once nsta <= 64 AND some low bit differs), forcing the first
    // block to sync station_live + the channelizer bin list.
    std::uint64_t live_mask_seen = ~std::uint64_t(0) - 1;

    // config `status.port` also carries the config/stations admin surface
    // (admin_routes.hpp). The write/raw endpoints need
    // FEEDMYFM_RX_ADMIN_PASSWORD; without it they refuse
    // (503) but the status + /api/stations/active polls still work. A
    // non-loopback status.host with no password is refused outright --
    // that would expose an unauthenticated writer... except it can't
    // write, but don't ship that shape.
    const char* admin_pw_env = std::getenv("FEEDMYFM_RX_ADMIN_PASSWORD");
    const std::string admin_pw = admin_pw_env ? admin_pw_env : "";
    const bool loopback = plan.status_host == "127.0.0.1" ||
                          plan.status_host == "::1" ||
                          plan.status_host == "localhost";
    if (plan.status_port > 0 && !loopback && admin_pw.empty()) {
        std::fprintf(stderr,
                     "fatal: status.host is %s (not loopback) but "
                     "FEEDMYFM_RX_ADMIN_PASSWORD is not set -- refusing to "
                     "expose the admin port without a password\n",
                     plan.status_host.c_str());
        return 1;
    }

    std::unique_ptr<fmrx::StatusServer> status;
    if (plan.status_port > 0) {
        auto routes = fmrx::admin_routes(config_path, stations_path, admin_pw, plan);
        if (spectrum) {
            const int scan_timeout_ms = plan.scan_average_ms + 2500;
            routes.push_back(
                {"GET", "/api/scan",
                 [&, scan_timeout_ms, admin_pw](const fmrx::HttpRequest& req)
                     -> fmrx::HttpResponse {
                     // Same admin_routes.hpp gate as everything else on
                     // this port: a scan ties up the RT loop's spectrum
                     // path for scan_average_ms and hands back RF detail
                     // (carrier freqs/levels) that, like the config/
                     // stations endpoints, has no business being
                     // reachable without the admin password.
                     if (admin_pw.empty())
                         return fmrx::HttpResponse::error(
                             503, "admin endpoints disabled: "
                                  "FEEDMYFM_RX_ADMIN_PASSWORD is not set "
                                  "on the receiver");
                     if (!fmrx::basic_auth_ok(req, admin_pw))
                         return fmrx::http_unauthorized();
                     const std::uint64_t want = scan_req.fetch_add(1) + 1;
                     for (int waited = 0; waited < scan_timeout_ms;
                          waited += 10) {
                         if (scan_done.load() >= want)
                             break;
                         std::this_thread::sleep_for(
                             std::chrono::milliseconds(10));
                     }
                     if (scan_done.load() < want)
                         return fmrx::HttpResponse::error(
                             503, "band scan did not complete -- SDR link "
                                  "down or receiver stalled");
                     std::vector<float> psd;
                     {
                         std::lock_guard<std::mutex> lk(scan_mtx);
                         psd = scan_psd;
                     }
                     const unsigned nf =
                         static_cast<unsigned>(plan.scan_nfft);
                     const auto carriers =
                         fmrx::detect_carriers(plan, nf, psd);
                     return fmrx::HttpResponse::json(
                         200, fmrx::scan_json(plan, nf, psd, carriers));
                 }});
        }
        // POST /active -- webui's relay pushes the set of stations that
        // have a live listener (by UDP port). Only wired when
        // decode.listener_gated is on. Loopback-trusted like the status
        // GET on this port (relay holds no admin password); a non-loopback
        // status.host additionally requires HTTP Basic, same as the write
        // endpoints -- and that shape already can't start without a
        // password set.
        if (gate) {
            std::unordered_map<int, int> port_to_k;
            for (int k = 0; k < nsta; ++k)
                port_to_k[plan.stations[k].port] = k;
            routes.push_back(
                {"POST", "/active",
                 [&active, port_to_k, admin_pw, loopback](
                     const fmrx::HttpRequest& req) -> fmrx::HttpResponse {
                     if (!loopback && !fmrx::basic_auth_ok(req, admin_pw))
                         return fmrx::http_unauthorized();
                     std::uint64_t mask = 0;
                     int matched = 0;
                     try {
                         const fmrx::yaml::Value body =
                             fmrx::yaml::Value::parse(req.body);
                         if (!body.is_map() || !body.has("ports"))
                             return fmrx::HttpResponse::error(
                                 400, "expected JSON {\"ports\": [..]}");
                         for (const fmrx::yaml::Value& p :
                              body.at("ports").seq()) {
                             auto it = port_to_k.find(p.as_int());
                             if (it == port_to_k.end())
                                 continue; // stale / unknown port -- ignore
                             mask |= std::uint64_t(1) << it->second;
                             ++matched;
                         }
                     } catch (const std::exception& e) {
                         return fmrx::HttpResponse::error(
                             400, std::string("bad request body: ") + e.what());
                     }
                     active.post(mask);
                     return fmrx::HttpResponse::json(
                         200, "{\"ok\":true,\"active\":" +
                                  std::to_string(matched) + "}");
                 }});
        }
        // GET /api/status/full -- the complete status frame regardless of
        // config `status.public.*`, for the authenticated /admin Signal
        // tab. Same gate as /api/scan; webui's
        // /admin/api/{path} proxy forwards the Authorization header.
        routes.push_back(
            {"GET", "/api/status/full",
             [&, admin_pw](const fmrx::HttpRequest& req) -> fmrx::HttpResponse {
                 if (admin_pw.empty())
                     return fmrx::HttpResponse::error(
                         503, "admin endpoints disabled: "
                              "FEEDMYFM_RX_ADMIN_PASSWORD is not set "
                              "on the receiver");
                 if (!fmrx::basic_auth_ok(req, admin_pw))
                     return fmrx::http_unauthorized();
                 return fmrx::HttpResponse::json(
                     200, status_json(
                              plan, stations, src, blocks, rt_ema.load(), gate,
                              gate && active.controlled(fmrx::ActiveSet::now_ms()),
                              sdr_peak_dbfs.load(), sdr_clip_ppm.load(),
                              fmrx::StatusPublic::all()));
             }});
        status = std::make_unique<fmrx::StatusServer>(
            plan.status_host, plan.status_port,
            [&] {
                return status_json(
                    plan, stations, src, blocks, rt_ema.load(), gate,
                    gate && active.controlled(fmrx::ActiveSet::now_ms()),
                    sdr_peak_dbfs.load(), sdr_clip_ppm.load(),
                    plan.status_public);
            },
            std::move(routes));
        std::fprintf(stderr,
                     "  admin endpoints: %s (config/stations GET+PUT+validate "
                     "on the same port)\n",
                     admin_pw.empty() ? "DISABLED (no FEEDMYFM_RX_ADMIN_PASSWORD)"
                                      : "enabled");
        std::fprintf(stderr, "  band scan: %s\n",
                     spectrum ? "GET /api/scan enabled"
                              : "disabled (scan.enabled: false)");
    }

    // --- composite ADC-headroom scan + software AGC loop --------------
    // The peak / clip scan runs every block regardless of gain_mode (it
    // feeds the status port and the clip alarm); the control law only
    // acts when gain_mode is auto_sw.
    const bool agc_sw = plan.sdr_gain_mode == "auto_sw";
    constexpr float kClip = 0.995f;                    // |re|,|im| at/above this = clipped
    const double kPeakEmaAlpha =                       // ~1.5 s time constant
        1.0 - std::exp(-block_seconds / 1.5);
    const double kClipEmaAlpha =                       // ~1 s
        1.0 - std::exp(-block_seconds / 1.0);
    double peak_ema_dbfs = -120.0;
    double clip_ppm_ema = 0.0;
    double agc_gain = static_cast<double>(plan.sdr_gain_db);   // current commanded gain
    const double agc_gain_ceil = static_cast<double>(plan.sdr_gain_db);
    auto agc_last_step = std::chrono::steady_clock::now();
    long agc_warmup_left =                             // ~2 s of EMA settle before first move
        agc_sw ? static_cast<long>(2.0 / block_seconds) + 1 : 0;
    if (agc_sw)
        std::fprintf(stderr,
                     "  front-end AGC: auto_sw (target %.0f dBFS, ceiling %.0f dB, "
                     "%.0f dB steps, %d ms)\n",
                     plan.sdr_agc_target_dbfs, agc_gain_ceil, plan.sdr_agc_step_db,
                     plan.sdr_agc_interval_ms);
    else
        std::fprintf(stderr, "  front-end AGC: gain_mode=%s (software loop off)\n",
                     plan.sdr_gain_mode.c_str());

    while (!g_stop.load()) {
        const cfloat* in = src.next_block();

        const bool link = src.link_up();
        if (link != last_link) {
            std::fprintf(stderr, "  sdr link %s\n", link ? "UP" : "DOWN");
            last_link = link;
        }

        // Band scan: feed the periodogram only while a request is pending.
        if (spectrum) {
            const std::uint64_t req = scan_req.load(std::memory_order_relaxed);
            if (req != scan_req_seen) {
                scan_req_seen = req;
                spectrum->reset();
                scan_feed_left = static_cast<int>(scan_feed_blocks);
            }
            if (scan_feed_left > 0) {
                spectrum->write(in, src.block_size());
                if (--scan_feed_left == 0) {
                    spectrum->get_psd(scan_scratch);
                    {
                        std::lock_guard<std::mutex> lk(scan_mtx);
                        scan_psd.swap(scan_scratch);
                    }
                    scan_done.store(scan_req_seen, std::memory_order_release);
                }
            }
        }

        // Composite ADC-headroom scan. One pass over the raw block: peak
        // max(|re|,|im|) and a hard-clip
        // count. max/fabs only, no sqrt -- auto-vectorises. Skipped while
        // the link is down (the reader feeds zeros, which would drag the
        // EMA to -inf dBFS and, under auto_sw, ramp the gain to the
        // ceiling against a dead antenna).
        if (link) {
            const size_t n = src.block_size();
            float pk = 0.0f;
            std::uint32_t clip = 0;
            for (size_t i = 0; i < n; ++i) {
                const float a = std::max(std::fabs(in[i].real()),
                                         std::fabs(in[i].imag()));
                pk = std::max(pk, a);
                clip += (a >= kClip);
            }
            const double pk_dbfs =
                20.0 * std::log10(std::max(pk, 1e-6f));
            const double ppm = 1e6 * static_cast<double>(clip) /
                               static_cast<double>(n);
            peak_ema_dbfs += kPeakEmaAlpha * (pk_dbfs - peak_ema_dbfs);
            clip_ppm_ema += kClipEmaAlpha * (ppm - clip_ppm_ema);
            sdr_peak_dbfs.store(peak_ema_dbfs);
            sdr_clip_ppm.store(clip_ppm_ema);

            // Software AGC control law. Peak-headroom regulation: move
            // the gain only when the composite
            // threatens the ADC or the whole band drops.
            if (agc_sw) {
                if (agc_warmup_left > 0) {
                    --agc_warmup_left;
                } else {
                    const auto now = std::chrono::steady_clock::now();
                    const double since_ms =
                        std::chrono::duration<double, std::milli>(
                            now - agc_last_step)
                            .count();
                    const bool hard_clip =
                        clip_ppm_ema > plan.sdr_agc_clip_ppm_trip;
                    double next = agc_gain;
                    if (peak_ema_dbfs > plan.sdr_agc_max_dbfs || hard_clip) {
                        // React fast; the hard-clip case skips the interval gate.
                        if (hard_clip || since_ms >= plan.sdr_agc_interval_ms)
                            next = agc_gain - plan.sdr_agc_step_db;
                    } else if (peak_ema_dbfs < plan.sdr_agc_target_dbfs -
                                                   plan.sdr_agc_hysteresis_db) {
                        if (since_ms >= plan.sdr_agc_interval_ms)
                            next = agc_gain + plan.sdr_agc_step_db;
                    }
                    next = std::clamp(next, plan.sdr_agc_min_gain_db,
                                      agc_gain_ceil);
                    if (next != agc_gain) {
                        agc_gain = next;
                        agc_last_step = now;
                        src.set_hardware_gain(agc_gain);
                    }
                }
            }
        }

        // Zero stations (stations.yml is empty / all-disabled / none in
        // span): receiver is up only for the band scan + admin endpoints.
        // Nothing consumes the channelizer, so skip both it and the pool;
        // the scan periodogram above is fed straight from `in`.
        if (nsta == 0) {
            ++blocks;
            rt_ema.store(0.98 * rt_ema.load());
            continue;
        }

        // Listener-gated decode: refresh which stations are live from the
        // relay's last POST /active (fail-open inside effective()). On a
        // change, rebuild station_live for the pool lambda and narrow the
        // channelizer copy-out to just the bins those stations read. Done
        // before channelize() so the new bin list takes effect this block.
        if (gate) {
            const std::uint64_t eff =
                active.effective(fmrx::ActiveSet::now_ms());
            if (eff != live_mask_seen) {
                live_mask_seen = eff;
                std::vector<int> live_bins;
                live_bins.reserve(static_cast<size_t>(nsta));
                for (int k = 0; k < nsta; ++k) {
                    const bool on = (eff >> k) & 1u;
                    station_live[static_cast<size_t>(k)] = on ? 1 : 0;
                    if (on)
                        live_bins.push_back(plan.stations[k].bin);
                }
                // channelizer.set_active_bins({}) means "all bins" -- with
                // zero listeners keep one bin in the list so the copy-out
                // stays narrow; the pool lambda still decodes none of it.
                if (live_bins.empty())
                    live_bins.push_back(plan.stations[0].bin);
                chan.set_active_bins(std::move(live_bins));
            }
        }

        auto t0 = std::chrono::steady_clock::now();
        chan.channelize(in, src.block_size());
        auto t1 = std::chrono::steady_clock::now();

        pool.run_block();
        auto t2 = std::chrono::steady_clock::now();

        ++blocks;
        rt_ema.store(0.98 * rt_ema.load() +
                     0.02 * (100.0 * std::chrono::duration<double>(t2 - t0).count() /
                             block_seconds));
        if (timing) {
            t_chan += std::chrono::duration<double>(t1 - t0).count();
            t_sta += std::chrono::duration<double>(t2 - t1).count();
            if (blocks % 125 == 0) {
                const double c = t_chan / 125, s = t_sta / 125;
                std::fprintf(stderr,
                             "  %lu blk  channelize %.2f ms  stations %.2f ms  "
                             "total %.2f / %.2f ms budget  (%.0f%% RT)  "
                             "overruns %lu  sdr %s\n",
                             blocks, c * 1e3, s * 1e3, (c + s) * 1e3,
                             block_seconds * 1e3,
                             100.0 * (c + s) / block_seconds, src.overruns(),
                             src.link_up() ? "up" : "DOWN");
                t_chan = t_sta = 0.0;
            }
        } else if (verbose && blocks % 125 == 0) {
            std::fprintf(stderr, "  %lu blocks\n", blocks);
        }
    }
    std::puts("\nstopped.");
    return 0;
}

// --optimize-cf: grid-search radio.center_freq to maximize the worst-case
// per-station distance from its channelizer bin edge, for a given
// config.yml + stations.yml. center_freq is a property of (station list,
// samp_rate, channelizer.num_channels, channelizer.oversample) together --
// re-run after any of those change. Only the frequency plan is computed
// (resolve.hpp); no SDR, no DSP.
int run_optimize_cf(const std::string& config_path,
                    const std::string& stations_path, long long step_hz,
                    long long range_hz, int top, bool allow_count_change) {
    std::string config_text, stations_text;
    try {
        config_text = fmrx::detail::read_file(config_path);
        stations_text = fmrx::detail::read_file(stations_path);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "fatal: %s\n", e.what());
        return 1;
    }

    // Split into lines, keeping trailing '\n' on each, so rewritten file is
    // byte-identical except for the one center_freq: value.
    std::vector<std::string> lines;
    for (size_t start = 0; start < config_text.size();) {
        size_t nl = config_text.find('\n', start);
        if (nl == std::string::npos) {
            lines.push_back(config_text.substr(start));
            break;
        }
        lines.push_back(config_text.substr(start, nl - start + 1));
        start = nl + 1;
    }

    int cf_idx = -1;
    for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
        size_t s = lines[i].find_first_not_of(" \t");
        if (s == std::string::npos) continue;
        if (lines[i].compare(s, 11, "center_freq") != 0) continue;
        size_t c = lines[i].find_first_not_of(" \t", s + 11);
        if (c != std::string::npos && lines[i][c] == ':') {
            cf_idx = i;
            break;
        }
    }
    if (cf_idx < 0) {
        std::fprintf(stderr, "fatal: no 'center_freq:' line in %s\n",
                     config_path.c_str());
        return 1;
    }

    const size_t colon = lines[cf_idx].find(':');
    long long base_cf = 0;
    {
        std::string digits;
        for (char c : lines[cf_idx].substr(colon + 1))
            if (c >= '0' && c <= '9') digits += c;
        if (digits.empty()) {
            std::fprintf(stderr, "fatal: could not parse the 'center_freq:' "
                                 "value in %s\n",
                         config_path.c_str());
            return 1;
        }
        base_cf = std::stoll(digits);
    }
    const std::string cf_prefix = lines[cf_idx].substr(0, colon + 1);
    auto with_cf = [&](long long cf) {
        std::string out;
        for (int i = 0; i < static_cast<int>(lines.size()); ++i)
            out += (i == cf_idx)
                       ? cf_prefix + " " + std::to_string(cf) + "\n"
                       : lines[i];
        return out;
    };

    struct Eval {
        int n = 0;
        double worst_margin = 0.0;
        std::string worst_label;
        int n_danger = 0;
    };
    auto evaluate = [&](long long cf) -> std::optional<Eval> {
        fmrx::ResolveResult r;
        try {
            r = fmrx::resolve_text(with_cf(cf), stations_text);
        } catch (const std::exception&) {
            return std::nullopt;
        }
        const fmrx::Plan& p = r.plan;
        if (p.stations.empty() || p.num_channels == 0) return std::nullopt;
        Eval e;
        e.n = static_cast<int>(p.stations.size());
        e.worst_margin = std::numeric_limits<double>::infinity();
        for (const auto& st : p.stations) {
            // Headroom between the carrier's far FM sideband and channel
            // Nyquist -- the quantity the roll-off finding is built on
            // (resolve.hpp). Tracks oversample (unlike bin_width/2) and
            // the station's real RF footprint (stereo / RDS widen it).
            const double channel_half_bw = st.channel_rate_hz / 2.0;
            const double carson_half = st.rf_occupied_bw_hz / 2.0;
            const double margin =
                channel_half_bw - (std::fabs(st.fine_hz) + carson_half);
            if (margin < e.worst_margin) {
                e.worst_margin = margin;
                e.worst_label = st.label;
            }
        }
        for (const std::string& f : r.findings)
            if (f.find("roll-off") != std::string::npos ||
                f.find("co-channel") != std::string::npos ||
                f.find("partly rejects") != std::string::npos)
                ++e.n_danger;
        return e;
    };

    long long bin_width_hz = 0;
    try {
        fmrx::ResolveResult br =
            fmrx::resolve_text(config_text, stations_text);
        bin_width_hz = br.plan.num_channels
                           ? br.plan.sdr_hw_rate_hz / br.plan.num_channels
                           : 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "fatal: current config is invalid: %s\n",
                     e.what());
        return 1;
    }
    if (range_hz < 0) range_hz = bin_width_hz;
    if (step_hz <= 0) {
        std::fprintf(stderr, "fatal: --step-hz must be > 0\n");
        return 1;
    }

    std::optional<Eval> baseline = evaluate(base_cf);
    if (!baseline) {
        std::fprintf(stderr,
                     "fatal: current center_freq leaves no stations in-band\n");
        return 1;
    }
    std::printf("Current center_freq=%.3f MHz: %d stations in-band, worst "
                "margin %.1f kHz (%s), %d/%d near an edge\n\n",
                base_cf / 1e6, baseline->n, baseline->worst_margin / 1e3,
                baseline->worst_label.c_str(), baseline->n_danger, baseline->n);

    struct Cand {
        long long cf;
        Eval e;
    };
    std::vector<Cand> results;
    for (long long cf = base_cf - range_hz; cf < base_cf + range_hz;
         cf += step_hz) {
        std::optional<Eval> e = evaluate(cf);
        if (!e) continue;
        if (!allow_count_change && e->n != baseline->n) continue;
        results.push_back({cf, *e});
    }
    if (results.empty()) {
        std::fprintf(stderr, "fatal: no candidates in range (try "
                             "--allow-station-count-change or a bigger "
                             "--range-hz)\n");
        return 1;
    }
    std::sort(results.begin(), results.end(), [](const Cand& a, const Cand& b) {
        return a.e.worst_margin > b.e.worst_margin;
    });

    const int shown = std::min(top, static_cast<int>(results.size()));
    std::printf("Top %d candidates by worst-case margin:\n", shown);
    for (int i = 0; i < shown; ++i) {
        const Cand& c = results[i];
        std::printf("  center_freq=%.3f MHz  in_band=%d  worst_margin=%5.1f kHz "
                    "(%s)  near_edge=%d/%d\n",
                    c.cf / 1e6, c.e.n, c.e.worst_margin / 1e3,
                    c.e.worst_label.c_str(), c.e.n_danger, c.e.n);
    }

    const Cand& best = results.front();
    if (best.e.worst_margin > baseline->worst_margin)
        std::printf("\nBest %.3f MHz improves the worst-case margin from %.1f "
                    "kHz to %.1f kHz.\n",
                    best.cf / 1e6, baseline->worst_margin / 1e3,
                    best.e.worst_margin / 1e3);
    else
        std::printf("\nCurrent center_freq=%.3f MHz is already at or near the "
                    "best margin in this range.\n",
                    base_cf / 1e6);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    Args args = parse_args(argc, argv);
    try {
        if (args.optimize_cf)
            return run_optimize_cf(args.config_path, args.stations_path,
                                   args.opt_step_hz, args.opt_range_hz,
                                   args.opt_top, args.opt_allow_count_change);

        fmrx::ResolveResult resolved =
            fmrx::resolve(args.config_path, args.stations_path);
        fmrx::Plan plan = std::move(resolved.plan);

        if (args.check && args.json) {
            print_check_json(plan, resolved.findings);
            return 0;
        }

        // --scan --json prints one JSON line and nothing else (scripts /
        // tests parse stdout); the plain form prints the findings first.
        if (args.scan && args.json)
            return fmrx::run_scan(plan, args.selftest_seconds < 0.2
                                      ? 0.35
                                      : args.selftest_seconds,
                            /*json=*/true);

        for (const std::string& f : resolved.findings)
            std::fprintf(stderr, "note: %s\n", f.c_str());

        if (args.scan)
            return fmrx::run_scan(plan, args.selftest_seconds < 0.2
                                      ? 0.35
                                      : args.selftest_seconds,
                            /*json=*/false);

        if (args.check) {
            std::fprintf(stderr,
                         "config OK: %zu station(s), %zu finding(s)\n",
                         plan.stations.size(), resolved.findings.size());
            return 0;
        }

        if (args.proto_m > 0) {
            std::fprintf(stderr, "override: channelizer proto_semilen_m %d -> %d\n",
                         plan.proto_semilen_m, args.proto_m);
            plan.proto_semilen_m = args.proto_m;
        }
        if (args.sweep)
            return fmrx::run_sweep(plan, args.selftest_seconds < 1.0
                                       ? 2.0
                                       : args.selftest_seconds);
        if (args.snr_cal)
            return fmrx::run_snr_cal(plan, args.selftest_seconds < 3.0
                                         ? 3.0
                                         : args.selftest_seconds);
        if (args.demod_cal)
            return fmrx::run_demod_cal(plan, args.selftest_seconds < 3.0
                                           ? 3.0
                                           : args.selftest_seconds);
        if (args.selftest)
            return fmrx::run_selftest(plan, args.selftest_seconds);
        if (args.stereo_selftest)
            return fmrx::run_stereo_selftest(plan, args.selftest_seconds < 1.0
                                                 ? 2.0
                                                 : args.selftest_seconds);

        if (args.watch) {
            std::fprintf(stderr, "watch: config auto-reload on -- edits to %s "
                                 "or %s restart the receiver\n",
                         args.config_path.c_str(), args.stations_path.c_str());
            start_config_watch(args.config_path, args.stations_path, argv);
        }
        return run_live(plan, args.verbose, args.threads, args.config_path,
                        args.stations_path);
    } catch (const std::exception& e) {
        if (args.check && args.json) {
            std::string o = "{\"ok\":false,\"error\":" +
                            fmrx::http_detail::json_quote(e.what()) + "}";
            std::puts(o.c_str());
        } else {
            std::fprintf(stderr, "fatal: %s\n", e.what());
        }
        return 1;
    }
}

// Synthesize a clean RDS composite (19 kHz pilot + 57 kHz DSB-SC biphase)
// and confirm RdsDecoder locks block sync and recovers PI / PS / PTY.
//
// Doubles as a regression guard for the front-end decimation in
// RdsDecoder: the decoder runs its recovery chain at a ~160 kHz working
// rate derived from channel_rate, so this exercises both the decimating
// path (channel_rate 640/320 kHz) and the pass-through path (128 kHz,
// below the decimation threshold) and asserts identical decoded output.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "rds_decoder.hpp"
#include "resolve.hpp"

using namespace fmrx;

namespace {

// ---- config: `rds` / `stereo` as global defaults, per-station overrides ----

const char* kBaseConfig = R"(
sdr: {ip: 10.0.0.10, gain_db: 23}
radio: {center_freq: 100000000, samp_rate: 16384000}
channelizer: {num_channels: 64, oversample: 2, atten_db: 60, proto_semilen_m: 8, block_size: 131072}
fm: {deviation: 75000, tau: 50e-6, intermediate_rate: 64000, predemod_transition: 20000}
audio: {rate: 32000, bandwidth: 15000, stop_bandwidth: 19000}
udp: {host: 0.0.0.0, mtu: 1316}
)";

// station A sets neither `rds:` nor `stereo:` (inherits both globals);
// station B forces both off.
const char* kStations = R"(
stations:
  - {label: A, freq: 94200000, port: 7001}
  - {label: B, freq: 96400000, port: 7002, rds: false, stereo: false}
)";

const char* name(StereoMode m) {
    return m == StereoMode::Auto ? "auto" : m == StereoMode::Stereo ? "on"
                                                                    : "off";
}

bool expect_rds(const std::string& cfg_extra, bool want_a, bool want_b) {
    const auto r = resolve_text(std::string(kBaseConfig) + cfg_extra, kStations);
    const bool a = r.plan.stations.at(0).rds_enabled;
    const bool b = r.plan.stations.at(1).rds_enabled;
    std::printf("  cfg[%-20s] -> A.rds=%d (want %d)  B.rds=%d (want %d)\n",
                cfg_extra.empty() ? "(none)" : cfg_extra.c_str(), a, want_a, b,
                want_b);
    return a == want_a && b == want_b;
}

bool expect_stereo(const std::string& cfg_extra, StereoMode want_a,
                   StereoMode want_b) {
    const auto r = resolve_text(std::string(kBaseConfig) + cfg_extra, kStations);
    const StereoMode a = r.plan.stations.at(0).stereo_mode;
    const StereoMode b = r.plan.stations.at(1).stereo_mode;
    std::printf("  cfg[%-20s] -> A.stereo=%s (want %s)  B.stereo=%s (want %s)\n",
                cfg_extra.empty() ? "(none)" : cfg_extra.c_str(), name(a),
                name(want_a), name(b), name(want_b));
    return a == want_a && b == want_b;
}

bool check_config_resolution() {
    std::printf("config: `rds` global default + per-station override\n");
    bool ok = true;
    ok &= expect_rds("", false, false);                     // default off
    ok &= expect_rds("rds: true\n", true, false);           // global on, B opts out
    ok &= expect_rds("rds: {enabled: true}\n", true, false); // mapping form
    ok &= expect_rds("rds: false\n", false, false);         // explicit off

    std::printf("config: `stereo` global mode default + per-station override\n");
    ok &= expect_stereo("", StereoMode::Mono, StereoMode::Mono);
    ok &= expect_stereo("stereo: auto\n", StereoMode::Auto, StereoMode::Mono);
    ok &= expect_stereo("stereo: true\n", StereoMode::Stereo, StereoMode::Mono);
    ok &= expect_stereo("stereo: {mode: auto, pilot_threshold_db: -28}\n",
                        StereoMode::Auto, StereoMode::Mono);
    return ok;
}

// One 0A group: PI in blocks A and C, PTY + segment index in B, two PS
// characters in D. `seg` (0..3) walks the 8-char PS name.
void push_group_0a(std::vector<uint32_t>& out, uint16_t pi, int pty,
                   const char* ps, int seg) {
    const uint16_t b_a = pi;
    const uint16_t b_b = static_cast<uint16_t>(
        (0x0u << 12) |                                    // group type 0
        (0u << 11) |                                      // version A
        (0u << 10) |                                      // TP
        ((static_cast<unsigned>(pty) & 0x1Fu) << 5) |     // PTY, bits 9..5
        (static_cast<unsigned>(seg) & 0x3u));             // TA/MS/DI/segment
    const uint16_t b_c = pi;                   // 0A block C = PI (again)
    const uint16_t b_d = static_cast<uint16_t>(
        (static_cast<uint8_t>(ps[seg * 2]) << 8) |
        static_cast<uint8_t>(ps[seg * 2 + 1]));
    out.push_back(rds::make_block(b_a, rds::kOffA));
    out.push_back(rds::make_block(b_b, rds::kOffB));
    out.push_back(rds::make_block(b_c, rds::kOffC));
    out.push_back(rds::make_block(b_d, rds::kOffD));
}

// One 2A group carrying RadioText chars [seg*4 .. seg*4+3]. `ab` is the
// A/B text flag; `text` is NUL-terminated and shorter than 64 -> a 0x0D
// terminator is emitted at strlen(text).
void push_group_2a(std::vector<uint32_t>& out, uint16_t pi, int ab, int seg,
                   const char* text) {
    auto chr = [&](int i) -> uint8_t {
        const int n = static_cast<int>(std::strlen(text));
        if (i < n) return static_cast<uint8_t>(text[i]);
        if (i == n) return 0x0D; // carriage-return terminator
        return ' ';
    };
    const uint16_t b_b = static_cast<uint16_t>(
        (0x2u << 12) | (0u << 11) | (0u << 10) |
        ((0u & 0x1Fu) << 5) |
        ((static_cast<unsigned>(ab) & 1u) << 4) |
        (static_cast<unsigned>(seg) & 0x0Fu));
    const uint16_t b_c =
        static_cast<uint16_t>((chr(seg * 4) << 8) | chr(seg * 4 + 1));
    const uint16_t b_d =
        static_cast<uint16_t>((chr(seg * 4 + 2) << 8) | chr(seg * 4 + 3));
    out.push_back(rds::make_block(pi, rds::kOffA));
    out.push_back(rds::make_block(b_b, rds::kOffB));
    out.push_back(rds::make_block(b_c, rds::kOffC));
    out.push_back(rds::make_block(b_d, rds::kOffD));
}

// Cheap deterministic white noise (xorshift -> [-1,1)), so a noisy test
// is reproducible across runs / machines.
struct Rng {
    uint64_t s = 0x9E3779B97F4A7C15ull;
    float next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return (static_cast<int64_t>(s) >> 11) * (1.0f / 4.5e15f);
    }
};

// Differentially encode + biphase-modulate the bitstream onto a 57 kHz
// DSB-SC subcarrier, add a 19 kHz pilot, optionally add white noise,
// return the real composite at Fs.
std::vector<float> synth_composite(const std::vector<int>& bits, double fs,
                                   float noise_rms = 0.0f) {
    const double fbit = 1187.5, fc = 57000.0, fp = 19000.0;
    const int spb = static_cast<int>(std::llround(fs / fbit));
    std::vector<float> comp;
    comp.reserve(bits.size() * static_cast<size_t>(spb) + spb);
    Rng rng;
    int diff = 0;
    long ns = 0;
    for (int b : bits) {
        diff ^= b; // differential encoder: line level is the running XOR
        const float lvl = diff ? 1.0f : -1.0f;
        for (int k = 0; k < spb; ++k) {
            const double t = static_cast<double>(ns) / fs;
            const float biphase = (k < spb / 2) ? lvl : -lvl;
            const float rds =
                biphase * static_cast<float>(std::cos(2 * M_PI * fc * t));
            const float pilot =
                0.30f * static_cast<float>(std::cos(2 * M_PI * fp * t));
            float x = 0.9f * rds + pilot;
            if (noise_rms > 0.0f)
                x += noise_rms * 1.732f * rng.next(); // ~unit-variance scale
            comp.push_back(x);
            ++ns;
        }
    }
    return comp;
}

bool run_one(double fs, uint16_t pi, int pty, const char* ps) {
    StationPlan sp;
    sp.channel_rate_hz = static_cast<int>(fs);
    sp.rds_enabled = true;
    RdsDecoder dec(sp);

    std::vector<uint32_t> blocks;
    for (int g = 0; g < 80; ++g)
        push_group_0a(blocks, pi, pty, ps, g & 3);

    std::vector<int> bits;
    bits.reserve(blocks.size() * 26);
    for (uint32_t blk : blocks)
        for (int i = 25; i >= 0; --i)
            bits.push_back((blk >> i) & 1);

    const std::vector<float> comp = synth_composite(bits, fs);
    for (size_t i = 0; i < comp.size(); i += 4096)
        dec.process(comp.data() + i,
                    std::min<size_t>(4096, comp.size() - i));

    const RdsPublic s = dec.snapshot();
    const int got_pi = s.pi < 0 ? 0 : s.pi;
    std::printf("  Fs=%7.0f  lock=%d  pi=0x%04X  pty=%d  ps=\"%s\"  "
                "groups_ok=%llu  ber=%.3f\n",
                fs, s.lock, got_pi, s.pty, s.ps.c_str(),
                static_cast<unsigned long long>(s.groups_ok),
                s.block_error_rate);

    bool ok = s.lock && got_pi == pi && s.pty == pty && s.groups_ok >= 20 &&
              s.block_error_rate < 0.1f && s.groups_per_sec > 10.0f;
    // PS publishes once it has been seen whole and stable; allow either the
    // final name or (on a short run) still-empty, but never a wrong one.
    if (!s.ps.empty() && s.ps != ps)
        ok = false;
    return ok;
}

// Same clean stream, but with white noise added: the front-end decimation
// (d9a0fce) must not have cost sensitivity -- it should still lock and
// recover PI/PS at a moderate C/N, with the group rate near the 11.4/s max.
bool check_noise(double fs, float noise_rms) {
    StationPlan sp;
    sp.channel_rate_hz = static_cast<int>(fs);
    sp.rds_enabled = true;
    RdsDecoder dec(sp);

    const uint16_t pi = 0x1234;
    std::vector<uint32_t> blocks;
    for (int g = 0; g < 200; ++g)
        push_group_0a(blocks, pi, 10, "TESTdata", g & 3);

    std::vector<int> bits;
    for (uint32_t blk : blocks)
        for (int i = 25; i >= 0; --i)
            bits.push_back((blk >> i) & 1);

    const std::vector<float> comp = synth_composite(bits, fs, noise_rms);
    for (size_t i = 0; i < comp.size(); i += 4096)
        dec.process(comp.data() + i, std::min<size_t>(4096, comp.size() - i));

    const RdsPublic s = dec.snapshot();
    const int got_pi = s.pi < 0 ? 0 : s.pi;
    std::printf("  Fs=%7.0f  noise_rms=%.2f -> lock=%d pi=0x%04X ps=\"%s\" "
                "gps=%.1f ber=%.2f\n",
                fs, noise_rms, s.lock, got_pi, s.ps.c_str(), s.groups_per_sec,
                s.block_error_rate);
    return s.lock && got_pi == pi && s.ps == "TESTdata" &&
           s.groups_per_sec > 9.0f && s.block_error_rate < 0.35f;
}

// bits -> composite -> dec.process(), in 4096-sample chunks.
void feed(RdsDecoder& dec, const std::vector<uint32_t>& blocks, double fs,
          float noise_rms = 0.0f) {
    std::vector<int> bits;
    bits.reserve(blocks.size() * 26);
    for (uint32_t blk : blocks)
        for (int i = 25; i >= 0; --i)
            bits.push_back((blk >> i) & 1);
    const std::vector<float> comp = synth_composite(bits, fs, noise_rms);
    for (size_t i = 0; i < comp.size(); i += 4096)
        dec.process(comp.data() + i, std::min<size_t>(4096, comp.size() - i));
}

// Feed `sec` seconds of pure noise (no RDS) so the decoder loses sync.
void feed_noise(RdsDecoder& dec, double fs, double sec, float rms) {
    Rng rng;
    const size_t n = static_cast<size_t>(fs * sec);
    std::vector<float> buf(4096);
    for (size_t done = 0; done < n; done += buf.size()) {
        for (float& v : buf)
            v = rms * 1.732f * rng.next();
        dec.process(buf.data(), std::min(buf.size(), n - done));
    }
}

// RadioText: full recovery, then the "hold the old text across an A/B
// flip until the new one is assembled" behaviour.
bool check_radiotext() {
    const double fs = 320000.0;
    const uint16_t pi = 0x1234;
    const char* T1 = "Grand duo concertant in Es op.48";
    const char* T2 = "Andreas Ottensamer (klarinet) Yuja Wang (piano)";
    const int nseg1 = (int)(std::strlen(T1) + 3) / 4 + 1;
    const int nseg2 = (int)(std::strlen(T2) + 3) / 4 + 1;

    StationPlan sp;
    sp.channel_rate_hz = (int)fs;
    sp.rds_enabled = true;
    RdsDecoder dec(sp);

    // lock + fully deliver T1 (A/B = 0), several clean passes
    std::vector<uint32_t> b;
    for (int g = 0; g < 40; ++g) push_group_0a(b, pi, 10, "TESTdata", g & 3);
    for (int pass = 0; pass < 4; ++pass)
        for (int s = 0; s < nseg1; ++s) push_group_2a(b, pi, 0, s, T1);
    feed(dec, b, fs);
    std::string rt1 = dec.snapshot().rt;
    bool ok = (rt1 == T1);
    std::printf("  after T1:            rt=\"%s\"  (want \"%s\")\n", rt1.c_str(), T1);

    // A/B flips to 1, new text starts arriving but only the first ~half of
    // the segments land -> RT must still read T1, not blank, not a fragment.
    b.clear();
    for (int g = 0; g < 8; ++g) push_group_0a(b, pi, 10, "TESTdata", g & 3);
    for (int pass = 0; pass < 2; ++pass)
        for (int s = 0; s < nseg2 / 2; ++s) push_group_2a(b, pi, 1, s, T2);
    feed(dec, b, fs);
    std::string rtmid = dec.snapshot().rt;
    std::printf("  mid-flip (T2 ~40%%):  rt=\"%s\"  (want held \"%s\")\n",
                rtmid.c_str(), T1);
    ok &= (rtmid == T1);

    // rest of T2 arrives -> RT converges to T2
    b.clear();
    for (int g = 0; g < 8; ++g) push_group_0a(b, pi, 10, "TESTdata", g & 3);
    for (int pass = 0; pass < 4; ++pass)
        for (int s = 0; s < nseg2; ++s) push_group_2a(b, pi, 1, s, T2);
    feed(dec, b, fs);
    std::string rt2 = dec.snapshot().rt;
    std::printf("  after T2 complete:   rt=\"%s\"  (want \"%s\")\n",
                rt2.c_str(), T2);
    ok &= (rt2 == T2);

    return ok;
}

// The >=3/4 gap-free-prefix reveal: a *fresh* RT delivered to ~80% of its
// segments should show a non-empty proper prefix of the target -- not the
// old text, not blank, not the whole string.
bool check_rt_partial_reveal() {
    const double fs = 320000.0;
    const uint16_t pi = 0x2222;
    const char* T = "Example Artist tenor Fictional Ensemble live in concert";
    const int nseg = (int)(std::strlen(T) + 3) / 4 + 1;

    StationPlan sp;
    sp.channel_rate_hz = (int)fs;
    sp.rds_enabled = true;
    RdsDecoder dec(sp);

    // lock, then deliver only the first ~80% of the (first ever) RT
    std::vector<uint32_t> b;
    for (int g = 0; g < 40; ++g) push_group_0a(b, pi, 10, "TESTdata", g & 3);
    const int upto = (nseg * 4) / 5;
    for (int pass = 0; pass < 4; ++pass)
        for (int s = 0; s < upto; ++s) push_group_2a(b, pi, 0, s, T);
    feed(dec, b, fs);

    const std::string rt = dec.snapshot().rt;
    const std::string full(T);
    const bool is_prefix =
        !rt.empty() && rt.size() < full.size() &&
        full.compare(0, rt.size(), rt) == 0;
    std::printf("  partial (80%% segs):  rt=\"%s\"  (want a proper prefix of T)\n",
                rt.c_str());
    return is_prefix;
}

// PS / RT are held over brief re-syncs but suppressed once sync has been
// gone > 5 s (retuned to a dead frequency).
bool check_unlock_clear() {
    const double fs = 320000.0;
    const uint16_t pi = 0x3333;
    const char* T = "Symfonie nr.9 in C D944";
    const int nseg = (int)(std::strlen(T) + 3) / 4 + 1;

    StationPlan sp;
    sp.channel_rate_hz = (int)fs;
    sp.rds_enabled = true;
    RdsDecoder dec(sp);

    std::vector<uint32_t> b;
    for (int g = 0; g < 40; ++g) push_group_0a(b, pi, 10, "TESTdata", g & 3);
    for (int pass = 0; pass < 5; ++pass)
        for (int s = 0; s < nseg; ++s) push_group_2a(b, pi, 0, s, T);
    feed(dec, b, fs);
    const RdsPublic before = dec.snapshot();

    // short noise burst (< 5 s) -> text is held
    feed_noise(dec, fs, 2.0, 0.5f);
    const RdsPublic brief = dec.snapshot();

    // long silence (> 5 s) -> text is dropped
    feed_noise(dec, fs, 8.0, 0.5f);
    const RdsPublic gone = dec.snapshot();

    std::printf("  locked:   ps=\"%s\" rt=\"%s\"\n", before.ps.c_str(),
                before.rt.c_str());
    std::printf("  +2s noise: ps=\"%s\" rt=\"%s\"  (want held)\n",
                brief.ps.c_str(), brief.rt.c_str());
    std::printf("  +10s noise: ps=\"%s\" rt=\"%s\"  (want empty)\n",
                gone.ps.c_str(), gone.rt.c_str());

    return before.ps == "TESTdata" && before.rt == T &&
           brief.ps == "TESTdata" && brief.rt == T &&
           gone.ps.empty() && gone.rt.empty();
}

} // namespace

int main() {
    const uint16_t kPI = 0x1234;
    const int kPTY = 10; // "Pop music"
    const char* kPS = "TESTdata";

    bool all = true;

    all &= check_config_resolution();

    std::printf("rds-selftest: synthetic 0A groups, PI 0x%04X PTY %d PS \"%s\"\n",
                kPI, kPTY, kPS);
    for (double fs : {640000.0, 320000.0, 128000.0})
        all &= run_one(fs, kPI, kPTY, kPS);

    std::printf("noise: lock + PI/PS through white noise (decimation sensitivity)\n");
    all &= check_noise(640000.0, 0.35f);
    all &= check_noise(320000.0, 0.35f);

    std::printf("radiotext: recovery + hold-across-A/B-flip\n");
    all &= check_radiotext();

    std::printf("radiotext: >=3/4 partial-prefix reveal\n");
    all &= check_rt_partial_reveal();

    std::printf("radiotext: PS/RT held over a brief re-sync, cleared after >5s\n");
    all &= check_unlock_clear();

    std::printf("RESULT: %s\n", all ? "PASS" : "FAIL");
    return all ? 0 : 1;
}

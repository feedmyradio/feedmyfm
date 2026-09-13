// The config / stations admin surface for the daemon's HTTP port. The
// route paths and JSON shapes are what webui's admin proxy
// (webui/routes/admin.py) expects, so it only has to point at this
// daemon's base URL:
//
//   GET  /api/config/raw        {"yaml": "<file>"}                 (auth)
//   POST /api/config/validate   {"ok": bool, "warnings"|"general_errors": ...}  (auth)
//   PUT  /api/config/raw        validate -> write, or 422 {"detail": ...} (auth)
//   GET  /api/stations/raw      ... same three, for stations.yml ...
//   POST /api/stations/validate
//   PUT  /api/stations/raw
//   GET  /api/stations/active   resolved plan for webui's relay        (no auth)
//
// Auth is HTTP Basic against `admin_password` (a single shared password,
// from FEEDMYFM_RX_ADMIN_PASSWORD). Empty password => the authed
// endpoints all refuse with 503; /api/stations/active still works.
//
// Validation and the active-plan resolve run in-process via
// resolve_text() -- no subprocess, no tempdir, and /api/stations/active
// is guaranteed consistent with what --check would say.
#pragma once

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "http_server.hpp"
#include "plan.hpp"
#include "resolve.hpp"
#include "yaml.hpp"

namespace fmrx {

namespace admin_detail {

inline std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Truncate-and-rewrite in place -- deliberately NOT tmp + rename. These
// files are single-file Docker bind mounts; a rename swaps the inode and
// the host's copy stops tracking the container's writes (and vice versa).
// An in-place write keeps the inode, so both sides stay in sync. A
// concurrent reader can see a short torn write, but the only reader is
// the --watch thread, which
// settles for 1 s and re-validates before acting, so a torn read is
// just retried on the next 2 s poll.
inline bool write_inplace(const std::string& path, const std::string& body,
                          std::string& err) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        err = "cannot open " + path + " for writing";
        return false;
    }
    f.write(body.data(), static_cast<std::streamsize>(body.size()));
    f.flush();
    if (!f) {
        err = "write to " + path + " failed";
        return false;
    }
    return true;
}

// Pull the "yaml" string out of a JSON body {"yaml": "..."}. JSON is
// valid YAML 1.2, so the vendored YAML parser handles it (escapes,
// unicode) rather than a hand-rolled scan. Throws on anything else.
inline std::string extract_yaml_field(const std::string& body) {
    yaml::Value v = yaml::Value::parse(body);
    if (!v.is_map() || !v.has("yaml"))
        throw std::runtime_error("expected a JSON body of the form "
                                 "{\"yaml\": \"...\"}");
    return v.at("yaml").as_string();
}

struct ValResult {
    bool ok = false;
    std::string json; // the full response body
};

// {"ok":true,"warnings":[...]}  or
// {"ok":false,"field_errors":[],"general_errors":["<message>"]}
inline ValResult validate(const std::string& config_text,
                          const std::string& stations_text) {
    ValResult r;
    try {
        ResolveResult rr = resolve_text(config_text, stations_text);
        std::string o = "{\"ok\":true,\"warnings\":[";
        for (size_t i = 0; i < rr.findings.size(); ++i)
            o += (i ? "," : "") + http_detail::json_quote(rr.findings[i]);
        o += "]}";
        r.ok = true;
        r.json = std::move(o);
    } catch (const std::exception& e) {
        r.ok = false;
        r.json = std::string("{\"ok\":false,\"field_errors\":[],"
                             "\"general_errors\":[") +
                 http_detail::json_quote(e.what()) + "]}";
    }
    return r;
}

// The GET /api/stations/active body: the *running* plan -- what rx is
// actually decoding right now, not a fresh re-resolve of whatever's
// currently on disk. Stations are sorted by frequency. A save that
// hasn't reached the DSP yet (no --watch, or still inside its ~2s poll)
// correctly doesn't show up here -- serving a re-resolve instead would
// let the relay bind UDP ports for stations the RT loop isn't decoding
// yet.
inline std::string active_plan_json(const Plan& p) {
    std::vector<const StationPlan*> ss;
    ss.reserve(p.stations.size());
    for (const auto& s : p.stations)
        ss.push_back(&s);
    std::sort(ss.begin(), ss.end(),
              [](const StationPlan* a, const StationPlan* b) {
                  return a->freq_hz < b->freq_hz;
              });

    std::string o = "{\"audio_rate\":" + std::to_string(p.audio_rate_hz);
    o += ",\"status_port\":" + std::to_string(p.status_port);
    o += ",\"listener\":{\"codec\":" + http_detail::json_quote(p.listener_codec);
    o += ",\"aac_bitrate_mono\":" + std::to_string(p.listener_aac_bitrate_mono);
    o += ",\"aac_bitrate_stereo\":" +
         std::to_string(p.listener_aac_bitrate_stereo) + "}";
    o += ",\"stations\":[";
    for (size_t i = 0; i < ss.size(); ++i) {
        const StationPlan& s = *ss[i];
        o += (i ? "," : "");
        o += "{\"port\":" + std::to_string(s.port);
        o += ",\"label\":" + http_detail::json_quote(s.label);
        o += ",\"freq\":" + std::to_string(s.freq_hz);
        o += ",\"stereo\":" + std::string(s.stereo ? "true" : "false");
        o += ",\"stereo_mode\":" +
             http_detail::json_quote(stereo_mode_str(s.stereo_mode));
        o += "}";
    }
    o += "]}";
    return o;
}

} // namespace admin_detail

// Build the admin route table. `config_path` / `stations_path` are the
// files served and (on PUT) rewritten. `admin_password` empty => the
// authed routes refuse with 503. `plan` is the daemon's running plan
// (resolved once at startup, or on a --watch reload) -- GET
// /api/stations/active serves it directly rather than re-resolving the
// files, so it can never show a station the RT loop isn't decoding.
inline std::vector<HttpRoute> admin_routes(std::string config_path,
                                           std::string stations_path,
                                           std::string admin_password,
                                           const Plan& plan) {
    namespace ad = admin_detail;

    // gate: 503 if no password configured, 401 if the request's is wrong.
    // Returns nullptr-equivalent (empty) on success; otherwise the
    // response to send.
    auto guard = [admin_password](const HttpRequest& req,
                                  HttpResponse& deny) -> bool {
        if (admin_password.empty()) {
            deny = HttpResponse::error(
                503, "admin endpoints disabled: FEEDMYFM_RX_ADMIN_PASSWORD "
                     "is not set on the receiver");
            return false;
        }
        if (!basic_auth_ok(req, admin_password)) {
            deny = http_unauthorized();
            return false;
        }
        return true;
    };

    // GET .../raw  -> {"yaml": "<file contents>"}
    auto make_get_raw = [guard](std::string path) {
        return [guard, path](const HttpRequest& req) {
            HttpResponse deny;
            if (!guard(req, deny))
                return deny;
            return HttpResponse::json(
                200, "{\"yaml\":" + http_detail::json_quote(ad::slurp(path)) +
                         "}");
        };
    };

    // POST .../validate  -> validation result for the posted draft,
    // checked against the *current* other file.
    auto make_validate = [guard](std::string this_path, std::string other_path,
                                 bool this_is_config) {
        return [guard, this_path, other_path, this_is_config](
                   const HttpRequest& req) {
            HttpResponse deny;
            if (!guard(req, deny))
                return deny;
            std::string draft;
            try {
                draft = ad::extract_yaml_field(req.body);
            } catch (const std::exception& e) {
                return HttpResponse::error(400, e.what());
            }
            const std::string other = ad::slurp(other_path);
            (void)this_path;
            ad::ValResult v = this_is_config ? ad::validate(draft, other)
                                             : ad::validate(other, draft);
            return HttpResponse::json(200, v.json);
        };
    };

    // PUT .../raw  -> validate; on success atomic-write and return the
    // result; on failure 422 {"detail": <result>} (the shape webui's
    // admin.js unwraps).
    auto make_put_raw = [guard](std::string this_path, std::string other_path,
                                bool this_is_config) {
        return [guard, this_path, other_path, this_is_config](
                   const HttpRequest& req) {
            HttpResponse deny;
            if (!guard(req, deny))
                return deny;
            std::string draft;
            try {
                draft = ad::extract_yaml_field(req.body);
            } catch (const std::exception& e) {
                return HttpResponse::error(400, e.what());
            }
            const std::string other = ad::slurp(other_path);
            ad::ValResult v = this_is_config ? ad::validate(draft, other)
                                             : ad::validate(other, draft);
            if (!v.ok)
                return HttpResponse::json(422, "{\"detail\":" + v.json + "}");
            std::string werr;
            if (!ad::write_inplace(this_path, draft, werr))
                return HttpResponse::error(500, werr);
            return HttpResponse::json(200, v.json);
        };
    };

    std::vector<HttpRoute> r;
    r.push_back({"GET", "/api/config/raw", make_get_raw(config_path)});
    r.push_back({"POST", "/api/config/validate",
                 make_validate(config_path, stations_path, true)});
    r.push_back({"PUT", "/api/config/raw",
                 make_put_raw(config_path, stations_path, true)});
    r.push_back({"GET", "/api/stations/raw", make_get_raw(stations_path)});
    r.push_back({"POST", "/api/stations/validate",
                 make_validate(stations_path, config_path, false)});
    r.push_back({"PUT", "/api/stations/raw",
                 make_put_raw(stations_path, config_path, false)});
    r.push_back({"GET", "/api/stations/active",
                 [&plan](const HttpRequest&) {
                     return HttpResponse::json(
                         200, admin_detail::active_plan_json(plan));
                 }});
    return r;
}

} // namespace fmrx

// The receiver's status + admin HTTP endpoint. `GET /` (or `GET
// /status`) returns the JSON from a body callback, built on demand from
// the live per-station atomics. `extra_routes` carries the config /
// stations admin surface (admin_routes.hpp) so one socket serves both.
//
// Bound to `bind_host` (config `status.host`, default 127.0.0.1). The
// receiver runs with network_mode: host so webui reaches it
// directly. Enabled by config `status.port` (0 = off). The status routes
// stay unauthenticated -- non-sensitive, and gating them would force
// every "what's active" poll through Basic auth.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "http_server.hpp"

namespace fmrx {

class StatusServer {
public:
    StatusServer(const std::string& bind_host, int port,
                 std::function<std::string()> body,
                 std::vector<HttpRoute> extra_routes = {}) {
        HttpHandler h = [body = std::move(body)](const HttpRequest&) {
            return HttpResponse::json(200, body());
        };
        std::vector<HttpRoute> routes{{"GET", "/", h}, {"GET", "/status", h}};
        for (auto& r : extra_routes)
            routes.push_back(std::move(r));
        m_server =
            std::make_unique<HttpServer>(bind_host, port, std::move(routes));
    }

    StatusServer(const StatusServer&) = delete;
    StatusServer& operator=(const StatusServer&) = delete;

private:
    std::unique_ptr<HttpServer> m_server;
};

} // namespace fmrx

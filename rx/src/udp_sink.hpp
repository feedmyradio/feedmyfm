// Fire-and-forget UDP sender for one station's int16 PCM stream. Chops
// the payload into datagrams that are a whole number of frames
// (frame_bytes = 2 for mono, 4 for interleaved stereo) and <= the MTU, so
// a frame is never split across two packets -- the webui relay forwards
// datagrams verbatim and the browser player treats the stream as tightly
// packed s16le frames.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

namespace fmrx {

class UdpSink {
public:
    UdpSink(const std::string& host, int port, int mtu, int frame_bytes = 2) {
        const size_t fb = static_cast<size_t>(std::max(2, frame_bytes));
        m_step = static_cast<size_t>(std::max<size_t>(1, mtu / fb)) * fb;
        // SOCK_CLOEXEC so --watch's execv() reload doesn't leak this fd
        // into the fresh image (16+ sinks leaked per reload otherwise).
        m_sock = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (m_sock < 0)
            throw std::runtime_error("UdpSink: socket() failed");
        std::memset(&m_addr, 0, sizeof(m_addr));
        m_addr.sin_family = AF_INET;
        m_addr.sin_port = htons(static_cast<uint16_t>(port));
        // 0.0.0.0 in config means "let the OS route it"; for a concrete
        // destination that has to be a real address, so treat the
        // wildcard as loopback -- consumers run on the same host.
        const char* dst = (host == "0.0.0.0" || host.empty()) ? "127.0.0.1"
                                                              : host.c_str();
        if (::inet_pton(AF_INET, dst, &m_addr.sin_addr) != 1) {
            ::close(m_sock);
            throw std::runtime_error("UdpSink: bad host '" + host + "'");
        }
    }

    ~UdpSink() {
        if (m_sock >= 0)
            ::close(m_sock);
    }

    UdpSink(const UdpSink&) = delete;
    UdpSink& operator=(const UdpSink&) = delete;
    UdpSink(UdpSink&& o) noexcept
        : m_sock(o.m_sock), m_addr(o.m_addr), m_step(o.m_step) {
        o.m_sock = -1;
    }

    void send_pcm(const int16_t* samples, size_t count) {
        const char* p = reinterpret_cast<const char*>(samples);
        const size_t total = count * sizeof(int16_t);
        for (size_t off = 0; off < total; off += m_step) {
            const size_t len = std::min(m_step, total - off);
            ::sendto(m_sock, p + off, len, 0,
                     reinterpret_cast<sockaddr*>(&m_addr), sizeof(m_addr));
        }
    }

private:
    int m_sock = -1;
    sockaddr_in m_addr{};
    size_t m_step = 2;
};

} // namespace fmrx

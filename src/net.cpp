#include "stratus/net.hpp"

#include <cstring>
#include <mutex>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace stratus::net {
namespace {

#ifdef _WIN32
constexpr Handle kInvalid = static_cast<Handle>(INVALID_SOCKET);
inline SOCKET raw(Handle h) { return static_cast<SOCKET>(h); }
inline void close_raw(Handle h) { ::closesocket(raw(h)); }
using socklen_type = int;
using buf_type = char*;
using cbuf_type = const char*;
#else
constexpr Handle kInvalid = -1;
inline int raw(Handle h) { return h; }
inline void close_raw(Handle h) { ::close(h); }
using socklen_type = socklen_t;
using buf_type = void*;
using cbuf_type = const void*;
#endif

void set_timeout(Handle h, int timeout_ms) {
    if (timeout_ms <= 0) return;
#ifdef _WIN32
    DWORD tv = static_cast<DWORD>(timeout_ms);
    ::setsockopt(raw(h), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
    ::setsockopt(raw(h), SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
    timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    ::setsockopt(raw(h), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(raw(h), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

}  // namespace

Handle invalid_handle() noexcept { return kInvalid; }

void startup() {
#ifdef _WIN32
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA data{};
        ::WSAStartup(MAKEWORD(2, 2), &data);
    });
#endif
}

void Socket::close() noexcept {
    if (h_ != kInvalid) {
        close_raw(h_);
        h_ = kInvalid;
    }
}

Socket listen_on(std::uint16_t port, int backlog) {
    startup();
    Handle h = static_cast<Handle>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (h == kInvalid) return Socket{};
    Socket s(h);

    int on = 1;
    ::setsockopt(raw(h), SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<cbuf_type>(&on), sizeof(on));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = ::htons(port);
    if (::bind(raw(h), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return Socket{};
    if (::listen(raw(h), backlog) != 0) return Socket{};
    return s;
}

Socket accept_one(const Socket& listener) {
    if (!listener.valid()) return Socket{};
    Handle c = static_cast<Handle>(::accept(raw(listener.get()), nullptr, nullptr));
    if (c == kInvalid) return Socket{};
    int on = 1;
    ::setsockopt(raw(c), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<cbuf_type>(&on), sizeof(on));
    return Socket(c);
}

std::vector<std::string> resolve_all(const std::string& host, std::uint16_t port) {
    startup();
    std::vector<std::string> out;
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const std::string service = std::to_string(port);
    if (::getaddrinfo(host.c_str(), service.c_str(), &hints, &res) != 0) return out;
    for (addrinfo* p = res; p != nullptr; p = p->ai_next) {
        char buf[INET6_ADDRSTRLEN]{};
        auto* in4 = reinterpret_cast<sockaddr_in*>(p->ai_addr);
        if (::inet_ntop(AF_INET, &in4->sin_addr, buf, sizeof(buf)) != nullptr) {
            std::string ip(buf);
            // getaddrinfo may repeat an address across socktypes; keep the set unique so
            // the proxy's replica count is the real one.
            bool seen = false;
            for (const auto& e : out) seen = seen || (e == ip);
            if (!seen) out.push_back(ip);
        }
    }
    ::freeaddrinfo(res);
    return out;
}

Socket connect_to(const std::string& host, std::uint16_t port, int timeout_ms) {
    startup();
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const std::string service = std::to_string(port);
    if (::getaddrinfo(host.c_str(), service.c_str(), &hints, &res) != 0) return Socket{};

    Socket out;
    for (addrinfo* p = res; p != nullptr; p = p->ai_next) {
        Handle h = static_cast<Handle>(::socket(p->ai_family, p->ai_socktype, p->ai_protocol));
        if (h == kInvalid) continue;
        Socket s(h);
        set_timeout(h, timeout_ms);
        if (::connect(raw(h), p->ai_addr, static_cast<socklen_type>(p->ai_addrlen)) == 0) {
            int on = 1;
            ::setsockopt(raw(h), IPPROTO_TCP, TCP_NODELAY,
                         reinterpret_cast<cbuf_type>(&on), sizeof(on));
            out = std::move(s);
            break;
        }
    }
    ::freeaddrinfo(res);
    return out;
}

bool send_all(const Socket& s, std::string_view data) {
    if (!s.valid()) return false;
    std::size_t sent = 0;
    while (sent < data.size()) {
        const auto n = ::send(raw(s.get()), data.data() + sent,
                              static_cast<int>(data.size() - sent), 0);
        if (n <= 0) return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

long recv_some(const Socket& s, char* buf, std::size_t cap, int timeout_ms) {
    if (!s.valid()) return -1;
    set_timeout(s.get(), timeout_ms);
    const auto n = ::recv(raw(s.get()), reinterpret_cast<buf_type>(buf),
                          static_cast<int>(cap), 0);
    return static_cast<long>(n);
}

std::string recv_until_close(const Socket& s, int timeout_ms) {
    std::string out;
    if (!s.valid()) return out;
    set_timeout(s.get(), timeout_ms);
    char buf[8192];
    for (;;) {
        const auto n = ::recv(raw(s.get()), reinterpret_cast<buf_type>(buf), sizeof(buf), 0);
        if (n <= 0) break;
        out.append(buf, static_cast<std::size_t>(n));
    }
    return out;
}

std::optional<http::ClientResponse> get(const std::string& host, std::uint16_t port,
                                        const std::string& path, int timeout_ms) {
    Socket s = connect_to(host, port, timeout_ms);
    if (!s.valid()) return std::nullopt;
    std::string req = "GET " + path + " HTTP/1.1\r\nHost: " + host + ":" +
                      std::to_string(port) + "\r\nConnection: close\r\n\r\n";
    if (!send_all(s, req)) return std::nullopt;
    const std::string raw_resp = recv_until_close(s, timeout_ms);
    if (raw_resp.empty()) return std::nullopt;
    return http::parse_response(raw_resp);
}

std::vector<Endpoint> parse_endpoints(std::string_view csv, std::uint16_t default_port) {
    std::vector<Endpoint> out;
    while (!csv.empty()) {
        const auto comma = csv.find(',');
        std::string_view item = csv.substr(0, comma);
        csv = (comma == std::string_view::npos) ? std::string_view{} : csv.substr(comma + 1);
        while (!item.empty() && (item.front() == ' ' || item.front() == '\t')) item.remove_prefix(1);
        while (!item.empty() && (item.back() == ' ' || item.back() == '\t' || item.back() == '\r'))
            item.remove_suffix(1);
        if (item.empty()) continue;

        Endpoint e;
        const auto colon = item.rfind(':');
        if (colon == std::string_view::npos) {
            e.host = std::string(item);
            e.port = default_port;
        } else {
            e.host = std::string(item.substr(0, colon));
            const std::string p(item.substr(colon + 1));
            unsigned long v = 0;
            try {
                v = std::stoul(p);
            } catch (...) {
                continue;
            }
            if (v == 0 || v > 65535) continue;
            e.port = static_cast<std::uint16_t>(v);
        }
        if (!e.host.empty()) out.push_back(std::move(e));
    }
    return out;
}

}  // namespace stratus::net

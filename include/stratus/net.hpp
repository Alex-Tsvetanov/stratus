// The only platform dependent surface in the project.
//
// Everything above this header is portable C++20. Winsock and the BSD sockets API differ
// in initialisation, in the handle type, in the close call and in the error reporting, so
// those four differences are absorbed here instead of being spread through the worker,
// the proxy, the load generator and the autoscaler.
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "stratus/http.hpp"

namespace stratus::net {

#ifdef _WIN32
using Handle = std::uintptr_t;
#else
using Handle = int;
#endif

Handle invalid_handle() noexcept;

// Process wide socket library initialisation. Idempotent, and a no-op off Windows.
void startup();

// Move only owner of a socket handle.
class Socket {
public:
    Socket() = default;
    explicit Socket(Handle h) : h_(h) {}
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& o) noexcept : h_(o.h_) { o.h_ = invalid_handle(); }
    Socket& operator=(Socket&& o) noexcept {
        if (this != &o) {
            close();
            h_ = o.h_;
            o.h_ = invalid_handle();
        }
        return *this;
    }
    ~Socket() { close(); }

    void close() noexcept;
    bool valid() const noexcept { return h_ != invalid_handle(); }
    Handle get() const noexcept { return h_; }
    Handle release() noexcept {
        Handle t = h_;
        h_ = invalid_handle();
        return t;
    }

private:
    Handle h_ = invalid_handle();
};

// Listening socket bound to every interface on port. Returns an invalid socket on failure.
Socket listen_on(std::uint16_t port, int backlog = 256);
Socket accept_one(const Socket& listener);

Socket connect_to(const std::string& host, std::uint16_t port, int timeout_ms = 5000);

bool send_all(const Socket& s, std::string_view data);
// One read. Returns 0 on orderly close and a negative value on error or timeout.
long recv_some(const Socket& s, char* buf, std::size_t cap, int timeout_ms = 5000);
// Reads until the peer closes or the deadline passes. The service answers with
// Connection: close, so close is the message boundary and no length parsing is needed.
std::string recv_until_close(const Socket& s, int timeout_ms = 5000);

// Every address a name resolves to. Docker's embedded DNS returns one A record per
// running replica of a compose service, which is what lets the proxy discover the fleet
// without a service registry.
std::vector<std::string> resolve_all(const std::string& host, std::uint16_t port);

// One shot HTTP GET. Returns nothing when the connection or the read failed.
std::optional<http::ClientResponse> get(const std::string& host, std::uint16_t port,
                                        const std::string& path, int timeout_ms = 5000);

// host:port, defaulting the port when absent.
struct Endpoint {
    std::string host;
    std::uint16_t port = 0;
};
std::vector<Endpoint> parse_endpoints(std::string_view csv, std::uint16_t default_port);

}  // namespace stratus::net

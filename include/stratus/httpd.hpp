// A blocking HTTP server with a fixed worker pool.
//
// A fixed pool rather than a thread per connection, because the whole experiment rests on
// knowing the concurrency of the service: with thread per connection the effective
// concurrency is whatever the client chose, the CPU oversubscribes, and the utilisation
// signal that drives the autoscaler stops meaning anything.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

#include "stratus/http.hpp"

namespace stratus::httpd {

using Handler = std::function<http::Response(const http::Request&)>;

struct Options {
    std::uint16_t port = 8080;
    int threads = 4;
    int read_timeout_ms = 15000;
};

// Runs until stop becomes true. Returns false when the port could not be bound.
bool serve(const Options& opts, const Handler& handler, std::atomic<bool>& stop);

}  // namespace stratus::httpd

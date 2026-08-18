#include "stratus/httpd.hpp"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "stratus/net.hpp"

namespace stratus::httpd {
namespace {

// Reads until the head is complete and the declared body has arrived. Requests to this
// service carry no body, so the second condition is satisfied immediately in practice,
// but honouring Content-Length costs three lines and avoids a truncated POST.
std::optional<http::Request> read_request(const net::Socket& s, int timeout_ms) {
    std::string buf;
    char chunk[4096];
    for (int guard = 0; guard < 4096; ++guard) {
        if (auto req = http::parse_request(buf)) {
            const auto it = req->headers.find("content-length");
            if (it == req->headers.end()) return req;
            std::size_t want = 0;
            try {
                want = static_cast<std::size_t>(std::stoull(it->second));
            } catch (...) {
                return req;
            }
            if (req->body.size() >= want) {
                req->body.resize(want);
                return req;
            }
        }
        const auto n = net::recv_some(s, chunk, sizeof(chunk), timeout_ms);
        if (n <= 0) return std::nullopt;
        buf.append(chunk, static_cast<std::size_t>(n));
    }
    return std::nullopt;
}

}  // namespace

bool serve(const Options& opts, const Handler& handler, std::atomic<bool>& stop) {
    net::startup();
    net::Socket listener = net::listen_on(opts.port);
    if (!listener.valid()) return false;

    std::mutex mu;
    std::condition_variable cv;
    std::deque<net::Handle> queue;

    const int nthreads = opts.threads > 0 ? opts.threads : 1;
    std::vector<std::thread> pool;
    pool.reserve(static_cast<std::size_t>(nthreads));

    for (int i = 0; i < nthreads; ++i) {
        pool.emplace_back([&] {
            for (;;) {
                net::Handle h{};
                {
                    std::unique_lock<std::mutex> lock(mu);
                    cv.wait(lock, [&] { return stop.load() || !queue.empty(); });
                    if (queue.empty()) {
                        if (stop.load()) return;
                        continue;
                    }
                    h = queue.front();
                    queue.pop_front();
                }
                net::Socket conn(h);
                auto req = read_request(conn, opts.read_timeout_ms);
                http::Response resp;
                if (!req) {
                    resp.status = 400;
                    resp.reason = "Bad Request";
                    resp.body = "malformed request\n";
                } else {
                    resp = handler(*req);
                }
                net::send_all(conn, http::serialise(resp));
            }
        });
    }

    while (!stop.load()) {
        net::Socket conn = net::accept_one(listener);
        if (!conn.valid()) continue;
        {
            std::lock_guard<std::mutex> lock(mu);
            queue.push_back(conn.release());
        }
        cv.notify_one();
    }

    cv.notify_all();
    for (auto& t : pool) t.join();
    return true;
}

}  // namespace stratus::httpd

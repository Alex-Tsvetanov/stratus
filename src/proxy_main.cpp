// stratus-proxy: round robin reverse proxy and metrics aggregator.
//
// Two jobs, one process, because they need the same thing: the current list of live
// worker replicas. Replicas are discovered by resolving the backend name on every
// refresh, which on a container network returns one address per running replica. There is
// no service registry and no configuration reload: scaling the service up or down changes
// what DNS answers, and the proxy follows.
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

#include "stratus/http.hpp"
#include "stratus/httpd.hpp"
#include "stratus/metrics.hpp"
#include "stratus/net.hpp"
#include "stratus/promparse.hpp"

namespace {

std::string env_or(const char* name, const std::string& fallback) {
    const char* v = std::getenv(name);
    return (v && *v) ? std::string(v) : fallback;
}

int env_int(const char* name, int fallback) {
    const char* v = std::getenv(name);
    if (!v || !*v) return fallback;
    try {
        return std::stoi(v);
    } catch (...) {
        return fallback;
    }
}

class Backends {
public:
    Backends(std::string host, std::uint16_t port, double ttl_s)
        : host_(std::move(host)), port_(port), ttl_(ttl_s) {}

    std::vector<std::string> current() {
        std::lock_guard<std::mutex> lock(mu_);
        const auto now = std::chrono::steady_clock::now();
        if (addrs_.empty() ||
            std::chrono::duration<double>(now - refreshed_).count() > ttl_) {
            auto found = stratus::net::resolve_all(host_, port_);
            if (!found.empty()) addrs_ = std::move(found);
            refreshed_ = now;
        }
        return addrs_;
    }

    // Round robin. A single counter shared by every request thread: the sequence is what
    // matters, not which thread got which index.
    std::string next(const std::vector<std::string>& addrs) {
        if (addrs.empty()) return {};
        const std::size_t i = cursor_.fetch_add(1, std::memory_order_relaxed);
        return addrs[i % addrs.size()];
    }

    std::uint16_t port() const noexcept { return port_; }

private:
    std::mutex mu_;
    std::string host_;
    std::uint16_t port_;
    double ttl_;
    std::vector<std::string> addrs_;
    std::chrono::steady_clock::time_point refreshed_{};
    std::atomic<std::size_t> cursor_{0};
};

struct ProxyState {
    stratus::metrics::Counter forwarded_ok;
    stratus::metrics::Counter forwarded_failed;
    stratus::metrics::Counter scrapes;
};

std::string aggregate_fleet(const std::vector<std::string>& docs, std::size_t backends,
                            const ProxyState& st) {
    std::map<std::string, stratus::prom::Meta> meta;
    std::map<std::string, double> totals;
    for (const auto& d : docs) {
        for (const auto& [name, m] : stratus::prom::parse_meta(d)) {
            if (meta[name].help.empty()) meta[name].help = m.help;
            if (meta[name].type.empty()) meta[name].type = m.type;
        }
        for (const auto& s : stratus::prom::parse(d)) {
            // Fold away the per replica identity: the controller scales the fleet, so the
            // fleet is the unit it must be able to read.
            totals[stratus::prom::strip_label(s.name, "instance")] += s.value;
        }
    }

    stratus::metrics::Exposition e;
    e.gauge("stratus_proxy_backends",
            "Worker replicas discovered by resolving the backend service name",
            static_cast<double>(backends));
    e.counter("stratus_proxy_forwarded_total", "Requests forwarded to a worker",
              st.forwarded_ok.get(), {{"outcome", "ok"}});
    e.counter("stratus_proxy_forwarded_total", "Requests forwarded to a worker",
              st.forwarded_failed.get(), {{"outcome", "error"}});
    e.counter("stratus_proxy_scrapes_total", "Fleet metric aggregations performed",
              st.scrapes.get());

    std::string last_metric;
    for (const auto& [series, value] : totals) {
        const std::string name = stratus::prom::metric_name(series);
        if (name != last_metric) {
            const auto it = meta.find(name);
            if (it != meta.end()) {
                if (!it->second.help.empty())
                    e.raw("# HELP " + name + " " + it->second.help + "\n");
                if (!it->second.type.empty())
                    e.raw("# TYPE " + name + " " + it->second.type + "\n");
            }
            last_metric = name;
        }
        e.raw(series + " " + stratus::metrics::fmt(value) + "\n");
    }
    return e.str();
}

}  // namespace

int main() {
    stratus::net::startup();

    const int port = env_int("STRATUS_PORT", 8080);
    const std::string backend_host = env_or("STRATUS_BACKEND_HOST", "worker");
    const int backend_port = env_int("STRATUS_BACKEND_PORT", 8081);
    const int threads = env_int("STRATUS_THREADS", 16);
    const double ttl = static_cast<double>(env_int("STRATUS_DISCOVERY_TTL_MS", 2000)) / 1000.0;

    Backends backends(backend_host, static_cast<std::uint16_t>(backend_port), ttl);
    ProxyState st;

    auto handler = [&](const stratus::http::Request& req) -> stratus::http::Response {
        stratus::http::Response resp;
        const auto addrs = backends.current();

        if (req.path == "/metrics") {
            st.scrapes.inc();
            std::vector<std::string> docs;
            docs.reserve(addrs.size());
            for (const auto& a : addrs) {
                if (auto r = stratus::net::get(a, backends.port(), "/metrics", 3000);
                    r && r->status == 200) {
                    docs.push_back(std::move(r->body));
                }
            }
            resp.content_type = "text/plain; version=0.0.4; charset=utf-8";
            resp.body = aggregate_fleet(docs, addrs.size(), st);
            return resp;
        }

        if (req.path == "/backends") {
            resp.body = std::to_string(addrs.size()) + " backends\n";
            for (const auto& a : addrs) resp.body += a + ":" + std::to_string(backends.port()) + "\n";
            return resp;
        }

        if (addrs.empty()) {
            st.forwarded_failed.inc();
            resp.status = 503;
            resp.reason = "Service Unavailable";
            resp.body = "no worker replicas resolved for " + backend_host + "\n";
            return resp;
        }

        const std::string target = backends.next(addrs);
        auto upstream = stratus::net::get(target, backends.port(), req.target, 60000);
        if (!upstream) {
            st.forwarded_failed.inc();
            resp.status = 502;
            resp.reason = "Bad Gateway";
            resp.body = "upstream " + target + " did not answer\n";
            return resp;
        }
        st.forwarded_ok.inc();
        resp.status = upstream->status;
        resp.reason = (upstream->status == 200) ? "OK" : "Upstream Status";
        resp.content_type = "application/json";
        resp.body = std::move(upstream->body);
        return resp;
    };

    std::cout << "stratus-proxy port=" << port << " backend=" << backend_host << ":"
              << backend_port << " threads=" << threads << std::endl;

    std::atomic<bool> stop{false};
    stratus::httpd::Options opts;
    opts.port = static_cast<std::uint16_t>(port);
    opts.threads = threads;
    if (!stratus::httpd::serve(opts, handler, stop)) {
        std::cerr << "stratus-proxy: could not bind port " << port << std::endl;
        return 1;
    }
    return 0;
}

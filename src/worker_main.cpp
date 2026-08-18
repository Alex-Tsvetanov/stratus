// stratus-worker: the compute service.
//
//   GET /healthz            liveness, answers as long as the accept loop is alive
//   GET /work?size=&iter=   one unit of work, CPU bound, size and ceiling parameterised
//   GET /metrics            Prometheus text exposition format
//
// Configuration comes from the environment, because the same image runs unchanged on the
// local stack and in a container platform, and the environment is the only configuration
// channel both of them agree on.
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "stratus/http.hpp"
#include "stratus/httpd.hpp"
#include "stratus/mandelbrot.hpp"
#include "stratus/metrics.hpp"

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

struct State {
    std::string instance;
    int threads = 1;
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();

    stratus::metrics::Counter requests_work_ok;
    stratus::metrics::Counter requests_work_bad;
    stratus::metrics::Counter requests_health;
    stratus::metrics::Counter requests_metrics;
    stratus::metrics::Counter iterations;
    stratus::metrics::Counter busy_seconds;
    stratus::metrics::Gauge inflight;
    // Bucket edges cover 1 ms to 8 s. Chosen once, before any measurement, so that the
    // exposed distribution cannot be tuned after seeing the answer.
    stratus::metrics::Histogram work_seconds{
        {0.001, 0.002, 0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.0, 4.0, 8.0}};
};

std::string render_metrics(const State& st) {
    using namespace stratus::metrics;
    Exposition e;
    const double up = std::chrono::duration<double>(
                          std::chrono::steady_clock::now() - st.started).count();

    e.gauge("stratus_worker_threads",
            "Size of the request handling pool, the capacity denominator for utilisation",
            static_cast<double>(st.threads), {{"instance", st.instance}});
    e.gauge("stratus_uptime_seconds", "Seconds since this worker started serving", up,
            {{"instance", st.instance}});
    e.gauge("stratus_inflight_requests", "Work requests currently being computed",
            st.inflight.get(), {{"instance", st.instance}});

    e.counter("stratus_busy_seconds_total",
              "Cumulative wall time spent inside the compute kernel, summed over threads",
              st.busy_seconds.get(), {{"instance", st.instance}});
    e.counter("stratus_work_iterations_total",
              "Cumulative inner loop iterations executed by the compute kernel",
              st.iterations.get(), {{"instance", st.instance}});

    e.counter("stratus_requests_total", "Requests served, by endpoint and outcome",
              st.requests_work_ok.get(),
              {{"instance", st.instance}, {"endpoint", "work"}, {"status", "200"}});
    e.counter("stratus_requests_total", "Requests served, by endpoint and outcome",
              st.requests_work_bad.get(),
              {{"instance", st.instance}, {"endpoint", "work"}, {"status", "400"}});
    e.counter("stratus_requests_total", "Requests served, by endpoint and outcome",
              st.requests_health.get(),
              {{"instance", st.instance}, {"endpoint", "healthz"}, {"status", "200"}});
    e.counter("stratus_requests_total", "Requests served, by endpoint and outcome",
              st.requests_metrics.get(),
              {{"instance", st.instance}, {"endpoint", "metrics"}, {"status", "200"}});

    e.histogram("stratus_work_duration_seconds", "Time to compute one unit of work",
                st.work_seconds, {{"instance", st.instance}});
    return e.str();
}

}  // namespace

int main() {
    State st;
    const int port = env_int("STRATUS_PORT", 8080);
    const int hw = static_cast<int>(std::thread::hardware_concurrency());
    st.threads = env_int("STRATUS_THREADS", hw > 0 ? hw : 4);
    st.instance = env_or("STRATUS_INSTANCE", env_or("HOSTNAME", "worker"));

    const int default_size = env_int("STRATUS_DEFAULT_SIZE", 256);
    const int default_iter = env_int("STRATUS_DEFAULT_ITER", 400);

    auto handler = [&st, default_size, default_iter](
                       const stratus::http::Request& req) -> stratus::http::Response {
        stratus::http::Response resp;

        if (req.path == "/healthz") {
            st.requests_health.inc();
            resp.body = "ok " + st.instance + "\n";
            return resp;
        }

        if (req.path == "/metrics") {
            st.requests_metrics.inc();
            resp.content_type = "text/plain; version=0.0.4; charset=utf-8";
            resp.body = render_metrics(st);
            return resp;
        }

        if (req.path == "/work") {
            const auto size = stratus::http::int_param(req, "size", default_size, 1, 4096);
            const auto iter = stratus::http::int_param(req, "iter", default_iter, 1, 100000);
            if (!size || !iter) {
                st.requests_work_bad.inc();
                resp.status = 400;
                resp.reason = "Bad Request";
                resp.body = "size must be 1..4096 and iter must be 1..100000\n";
                return resp;
            }

            st.inflight.add(1.0);
            const auto t0 = std::chrono::steady_clock::now();
            const auto result = stratus::compute(static_cast<std::uint32_t>(*size),
                                                 static_cast<std::uint32_t>(*iter));
            const double elapsed =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            st.inflight.add(-1.0);

            st.busy_seconds.inc(elapsed);
            st.iterations.inc(static_cast<double>(result.iterations));
            st.work_seconds.observe(elapsed);
            st.requests_work_ok.inc();

            resp.content_type = "application/json";
            resp.body = "{\"instance\":\"" + st.instance + "\",\"size\":" +
                        std::to_string(*size) + ",\"iter\":" + std::to_string(*iter) +
                        ",\"checksum\":" + std::to_string(result.checksum) +
                        ",\"iterations\":" + std::to_string(result.iterations) +
                        ",\"seconds\":" + stratus::metrics::fmt(elapsed) + "}\n";
            return resp;
        }

        resp.status = 404;
        resp.reason = "Not Found";
        resp.body = "endpoints: /healthz /work /metrics\n";
        return resp;
    };

    std::atomic<bool> stop{false};
    stratus::httpd::Options opts;
    opts.port = static_cast<std::uint16_t>(port);
    opts.threads = st.threads;

    std::cout << "stratus-worker instance=" << st.instance << " port=" << port
              << " threads=" << st.threads << " default_size=" << default_size
              << " default_iter=" << default_iter << std::endl;

    if (!stratus::httpd::serve(opts, handler, stop)) {
        std::cerr << "stratus-worker: could not bind port " << port << std::endl;
        return 1;
    }
    return 0;
}

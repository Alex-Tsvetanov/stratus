// stratus-loadgen: synthetic load with configurable concurrency and rate.
//
// Two modes, because they answer different questions. With --rate 0 the generator is
// closed loop: N threads each send the next request as soon as the previous one returns,
// which measures how much work the service can absorb. With --rate R it is open loop: the
// send schedule is fixed in advance, so a service that slows down accumulates queueing
// delay in the reported latency instead of quietly throttling the client. The second mode
// is the one that shows what the autoscaler is for.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "stratus/args.hpp"
#include "stratus/http.hpp"
#include "stratus/net.hpp"
#include "stratus/stats.hpp"

namespace {

using Clock = std::chrono::steady_clock;

struct Sample {
    double t_offset_s = 0.0;
    double latency_s = 0.0;
    bool ok = false;
};

void usage() {
    std::cout <<
        "stratus-loadgen --host H --port P [options]\n"
        "  --path S        request target (default /work)\n"
        "  --size N        work size parameter appended to the target (default 256)\n"
        "  --iter N        iteration ceiling appended to the target (default 400)\n"
        "  --concurrency N in flight requests (default 8)\n"
        "  --rate R        target requests per second, 0 for closed loop (default 0)\n"
        "  --duration S    measurement seconds (default 20)\n"
        "  --warmup S      discarded seconds before measuring (default 3)\n"
        "  --csv FILE      write per request samples\n";
}

}  // namespace

int main(int argc, char** argv) {
    stratus::args::Args a(argc, argv);
    if (a.has("help")) {
        usage();
        return 0;
    }

    const std::string host = a.str_or("host", "127.0.0.1");
    const auto port = static_cast<std::uint16_t>(a.int_or("port", 8080));
    const std::string path = a.str_or("path", "/work");
    const int size = a.int_or("size", 256);
    const int iter = a.int_or("iter", 400);
    const int concurrency = std::max(1, a.int_or("concurrency", 8));
    const double rate = a.num_or("rate", 0.0);
    const double duration = a.num_or("duration", 20.0);
    const double warmup = a.num_or("warmup", 3.0);
    const std::string csv = a.str_or("csv", "");

    const std::string target = path + "?size=" + std::to_string(size) +
                               "&iter=" + std::to_string(iter);

    stratus::net::startup();

    std::vector<std::vector<Sample>> per_thread(static_cast<std::size_t>(concurrency));
    std::atomic<bool> running{true};
    std::atomic<long long> issued{0};

    const auto t_start = Clock::now();
    const auto t_end = t_start + std::chrono::duration_cast<Clock::duration>(
                                     std::chrono::duration<double>(warmup + duration));

    auto worker = [&](int idx) {
        auto& out = per_thread[static_cast<std::size_t>(idx)];
        out.reserve(4096);
        while (running.load(std::memory_order_relaxed)) {
            if (rate > 0.0) {
                // Open loop. The nth request is due at n / rate from the start, computed
                // from a shared counter so the whole client obeys one schedule.
                const long long n = issued.fetch_add(1, std::memory_order_relaxed);
                const auto due = t_start + std::chrono::duration_cast<Clock::duration>(
                                               std::chrono::duration<double>(
                                                   static_cast<double>(n) / rate));
                if (due > t_end) break;
                std::this_thread::sleep_until(due);
            }
            const auto t0 = Clock::now();
            if (t0 >= t_end) break;
            auto resp = stratus::net::get(host, port, target, 60000);
            const auto t1 = Clock::now();

            Sample s;
            s.t_offset_s = std::chrono::duration<double>(t0 - t_start).count();
            s.latency_s = std::chrono::duration<double>(t1 - t0).count();
            s.ok = resp.has_value() && resp->status == 200;
            out.push_back(s);
        }
    };

    std::vector<std::thread> pool;
    pool.reserve(static_cast<std::size_t>(concurrency));
    for (int i = 0; i < concurrency; ++i) pool.emplace_back(worker, i);

    std::this_thread::sleep_until(t_end);
    running.store(false);
    for (auto& t : pool) t.join();

    // Discard the warmup window: the first requests pay for cold caches, for lazily
    // created connections and, when the stack was just started, for containers that are
    // still coming up. Including them would report a system that does not exist after the
    // first three seconds.
    std::vector<Sample> all;
    for (auto& v : per_thread)
        for (auto& s : v)
            if (s.t_offset_s >= warmup) all.push_back(s);

    if (!csv.empty()) {
        std::ofstream f(csv);
        f << "offset_s,latency_s,ok\n";
        for (const auto& s : all)
            f << s.t_offset_s << ',' << s.latency_s << ',' << (s.ok ? 1 : 0) << '\n';
    }

    std::vector<double> ok_lat;
    long long failed = 0;
    for (const auto& s : all) {
        if (s.ok) ok_lat.push_back(s.latency_s);
        else ++failed;
    }
    std::sort(ok_lat.begin(), ok_lat.end());

    const double measured = duration;
    const double thr = static_cast<double>(ok_lat.size()) / (measured > 0 ? measured : 1.0);

    auto ms = [](double s) { return s * 1000.0; };
    std::printf("\n=== stratus-loadgen ===\n");
    std::printf("target            http://%s:%u%s\n", host.c_str(), static_cast<unsigned>(port),
                target.c_str());
    std::printf("concurrency       %d\n", concurrency);
    std::printf("rate requested    %s\n", rate > 0.0 ? std::to_string(rate).c_str() : "closed loop");
    std::printf("warmup / measure  %.1f s / %.1f s\n", warmup, measured);
    std::printf("requests ok       %zu\n", ok_lat.size());
    std::printf("requests failed   %lld\n", failed);
    std::printf("throughput        %.2f req/s\n", thr);
    std::printf("work rate         %.2f Miter/s\n",
                thr * static_cast<double>(size) * static_cast<double>(size) *
                    static_cast<double>(iter) / 1e6);
    std::printf("latency mean      %.2f ms\n", ms(stratus::stats::mean(ok_lat)));
    std::printf("latency p50       %.2f ms\n", ms(stratus::stats::percentile_sorted(ok_lat, 0.50)));
    std::printf("latency p90       %.2f ms\n", ms(stratus::stats::percentile_sorted(ok_lat, 0.90)));
    std::printf("latency p95       %.2f ms\n", ms(stratus::stats::percentile_sorted(ok_lat, 0.95)));
    std::printf("latency p99       %.2f ms\n", ms(stratus::stats::percentile_sorted(ok_lat, 0.99)));
    std::printf("latency max       %.2f ms\n", ok_lat.empty() ? 0.0 : ms(ok_lat.back()));
    if (!csv.empty()) std::printf("samples written   %s\n", csv.c_str());
    std::printf("\n");

    return ok_lat.empty() ? 1 : 0;
}

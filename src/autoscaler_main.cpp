// stratus-autoscaler: the control loop.
//
// Scrape, derive utilisation from the counter delta, decide, act, record. The decision
// itself is a pure function in include/stratus/scaling.hpp and is unit tested there; this
// file is the part that touches the world, and it is kept deliberately thin so that the
// interesting logic is not trapped behind a running container stack.
//
// Utilisation is derived the way a scraper derives a rate, from two consecutive readings
// of a monotonic counter:
//
//     utilisation = d(busy_seconds) / (d(wall_seconds) * worker_threads_total)
//
// CPU load average is deliberately not used. The workload saturates a core by design, so
// per core load is close to one whether the fleet is comfortable or drowning, and it
// carries no signal about whether more replicas are needed.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "stratus/args.hpp"
#include "stratus/net.hpp"
#include "stratus/promparse.hpp"
#include "stratus/scaling.hpp"

namespace {

using Clock = std::chrono::steady_clock;

struct Reading {
    bool ok = false;
    double busy_seconds = 0.0;
    double worker_threads = 0.0;
    int backends = 0;
    double inflight = 0.0;
    double requests_ok = 0.0;
};

Reading scrape(const std::string& host, std::uint16_t port) {
    Reading r;
    auto resp = stratus::net::get(host, port, "/metrics", 5000);
    if (!resp || resp->status != 200) return r;
    const auto totals = stratus::prom::aggregate({resp->body});

    double v = 0.0;
    for (const auto& [series, value] : totals) {
        const std::string name = stratus::prom::metric_name(series);
        if (name == "stratus_busy_seconds_total") r.busy_seconds += value;
        else if (name == "stratus_worker_threads") r.worker_threads += value;
        else if (name == "stratus_inflight_requests") r.inflight += value;
        else if (name == "stratus_requests_total" &&
                 series.find("endpoint=\"work\"") != std::string::npos &&
                 series.find("status=\"200\"") != std::string::npos)
            r.requests_ok += value;
    }
    if (stratus::prom::find(totals, "stratus_proxy_backends", v)) r.backends = static_cast<int>(v);
    r.ok = true;
    return r;
}

void usage() {
    std::cout <<
        "stratus-autoscaler [options]\n"
        "  --host H            proxy host exposing the aggregated /metrics (default 127.0.0.1)\n"
        "  --port P            proxy port (default 8080)\n"
        "  --service NAME      compose service to scale (default worker)\n"
        "  --compose-file F    compose file passed to docker compose (default docker-compose.yml)\n"
        "  --project NAME      compose project name (default stratus)\n"
        "  --target U          target utilisation, 0..1 (default 0.7)\n"
        "  --min N / --max N   replica bounds (default 1 / 6)\n"
        "  --interval S        control period in seconds (default 3)\n"
        "  --duration S        how long to run, 0 for forever (default 0)\n"
        "  --cooldown-up S     seconds before another scale up (default 6)\n"
        "  --cooldown-down S   seconds before a scale down (default 20)\n"
        "  --csv FILE          write the decision timeline\n"
        "  --dry-run           decide and print, never call docker\n";
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
    const std::string service = a.str_or("service", "worker");
    const std::string compose_file = a.str_or("compose-file", "docker-compose.yml");
    const std::string project = a.str_or("project", "stratus");
    const double interval = a.num_or("interval", 3.0);
    const double duration = a.num_or("duration", 0.0);
    const std::string csv = a.str_or("csv", "");
    const bool dry_run = a.has("dry-run");

    stratus::scaling::Policy policy;
    policy.target_utilisation = a.num_or("target", 0.70);
    policy.min_replicas = a.int_or("min", 1);
    policy.max_replicas = a.int_or("max", 6);
    policy.scale_up_cooldown_s = a.num_or("cooldown-up", 6.0);
    policy.scale_down_cooldown_s = a.num_or("cooldown-down", 20.0);

    stratus::net::startup();

    std::ofstream timeline;
    if (!csv.empty()) {
        timeline.open(csv);
        timeline << "t_s,replicas,utilisation,raw_desired,desired,action,reason,rps\n";
    }

    std::printf("stratus-autoscaler target=%.2f bounds=[%d,%d] interval=%.1fs "
                "cooldown up/down=%.0f/%.0fs%s\n",
                policy.target_utilisation, policy.min_replicas, policy.max_replicas,
                interval, policy.scale_up_cooldown_s, policy.scale_down_cooldown_s,
                dry_run ? " (dry run)" : "");
    std::printf("%8s %9s %12s %9s %11s  %s\n", "t[s]", "replicas", "utilisation",
                "desired", "action", "reason");

    const auto t0 = Clock::now();
    auto last_change = t0;
    Reading prev = scrape(host, port);
    auto prev_time = Clock::now();

    for (;;) {
        std::this_thread::sleep_for(std::chrono::duration<double>(interval));
        const auto now = Clock::now();
        const double t_rel = std::chrono::duration<double>(now - t0).count();
        if (duration > 0.0 && t_rel > duration) break;

        const Reading cur = scrape(host, port);
        if (!cur.ok) {
            std::printf("%8.1f %9s %12s %9s %11s  %s\n", t_rel, "-", "-", "-", "error",
                        "metrics endpoint unreachable");
            continue;
        }
        const double wall = std::chrono::duration<double>(now - prev_time).count();
        const double busy_delta = cur.busy_seconds - prev.busy_seconds;
        const double req_delta = cur.requests_ok - prev.requests_ok;

        // A fleet total is the sum of per replica counters, so removing a replica makes it
        // fall. That is a counter reset, and a reset interval carries no usable rate: the
        // first version of this loop read the negative delta as negative utilisation and
        // decided, correctly given the input and uselessly given reality, to scale down.
        // The baseline moves forward and the interval is skipped, which is what a scraper
        // does with a reset.
        if (busy_delta < 0.0 || req_delta < 0.0) {
            std::printf("%8.1f %9d %12s %9s %11s  %s\n", t_rel, cur.backends, "-", "-",
                        "skip", "counter reset, fleet size changed");
            std::fflush(stdout);
            prev = cur;
            prev_time = now;
            continue;
        }

        // Threads are averaged over the interval: the fleet may have changed size inside
        // it, and using only the current value would attribute the whole interval's busy
        // time to the wrong capacity.
        const double threads_avg = (prev.worker_threads + cur.worker_threads) / 2.0;
        const double util = stratus::scaling::utilisation_from_delta(
            busy_delta, wall, static_cast<int>(threads_avg + 0.5));
        const double rps = wall > 0.0 ? req_delta / wall : 0.0;

        const int replicas = cur.backends > 0 ? cur.backends : 1;
        const double since = std::chrono::duration<double>(now - last_change).count();
        const auto decision = stratus::scaling::decide(policy, replicas, util, since);

        std::printf("%8.1f %9d %12.3f %9d %11s  %s (%.1f req/s)\n", t_rel, replicas, util,
                    decision.desired_replicas, stratus::scaling::action_name(decision.action),
                    decision.reason.c_str(), rps);
        std::fflush(stdout);

        if (timeline.is_open()) {
            timeline << t_rel << ',' << replicas << ',' << util << ',' << decision.raw_desired
                     << ',' << decision.desired_replicas << ','
                     << stratus::scaling::action_name(decision.action) << ",\""
                     << decision.reason << "\"," << rps << '\n';
            timeline.flush();
        }

        if (decision.action != stratus::scaling::Action::Hold) {
            const std::string cmd = "docker compose -p " + project + " -f " + compose_file +
                                    " up -d --no-recreate --scale " + service + "=" +
                                    std::to_string(decision.desired_replicas) +
                                    " " + service;
            if (dry_run) {
                std::printf("         would run: %s\n", cmd.c_str());
            } else {
                // Compose narrates every container it touches. Left on the terminal it
                // buries the decision timeline, which is the output that matters here.
#ifdef _WIN32
                const std::string quiet = cmd + " >NUL 2>&1";
#else
                const std::string quiet = cmd + " >/dev/null 2>&1";
#endif
                const int rc = std::system(quiet.c_str());
                if (rc != 0) std::printf("         scale command failed, rc=%d\n", rc);
            }
            last_change = now;
        }

        prev = cur;
        prev_time = now;
    }

    return 0;
}

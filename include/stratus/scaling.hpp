// The autoscaling policy.
//
// This is the part a cloud provider normally hides. It is a pure function of the observed
// utilisation and the elapsed time since the last change, which is exactly why it is
// worth extracting: a control loop that can only be observed by watching a live cluster
// cannot be argued about, and cannot be tested.
//
// The rule is target tracking with a dead band and asymmetric cooldowns:
//
//     desired = ceil(replicas * utilisation / target)
//
// clamped to [min, max]. Scaling up is allowed after a short cooldown, scaling down only
// after a longer one, because the cost of reacting late to a load increase is paid by
// every request in the queue, while the cost of releasing a worker late is one worker.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace stratus::scaling {

struct Policy {
    double target_utilisation = 0.70;  // busy fraction of worker capacity we aim for
    int min_replicas = 1;
    int max_replicas = 8;
    double scale_up_cooldown_s = 5.0;
    double scale_down_cooldown_s = 20.0;
    // Relative dead band. A desired count within this fraction of the current one is
    // treated as noise. Without it the loop oscillates by one replica indefinitely.
    double dead_band = 0.10;
};

enum class Action { Hold, ScaleUp, ScaleDown };

struct Decision {
    Action action = Action::Hold;
    int desired_replicas = 0;
    double utilisation = 0.0;
    double raw_desired = 0.0;
    std::string reason;
};

inline const char* action_name(Action a) {
    switch (a) {
        case Action::ScaleUp: return "scale-up";
        case Action::ScaleDown: return "scale-down";
        default: return "hold";
    }
}

// utilisation is the observed busy fraction of the fleet, in [0, inf). Values above 1.0
// are possible and meaningful: they mean the queue is growing.
inline Decision decide(const Policy& p, int current_replicas, double utilisation,
                       double seconds_since_last_change) {
    Decision d;
    d.utilisation = utilisation;
    d.desired_replicas = current_replicas;

    const int cur = std::max(1, current_replicas);
    const double target = (p.target_utilisation > 0.0) ? p.target_utilisation : 1.0;

    d.raw_desired = static_cast<double>(cur) * utilisation / target;
    int desired = static_cast<int>(std::ceil(d.raw_desired - 1e-9));
    desired = std::clamp(desired, p.min_replicas, p.max_replicas);

    if (desired == current_replicas) {
        d.reason = "at target";
        return d;
    }

    // Dead band, expressed on the unclamped desire so that a request to move from 3.05 to
    // 3 does not count as a decision.
    const double relative = std::abs(d.raw_desired - static_cast<double>(cur)) /
                            static_cast<double>(cur);
    if (relative < p.dead_band) {
        d.reason = "within dead band";
        return d;
    }

    const bool up = desired > current_replicas;
    const double cooldown = up ? p.scale_up_cooldown_s : p.scale_down_cooldown_s;
    if (seconds_since_last_change < cooldown) {
        d.reason = up ? "scale-up blocked by cooldown" : "scale-down blocked by cooldown";
        return d;
    }

    d.action = up ? Action::ScaleUp : Action::ScaleDown;
    d.desired_replicas = desired;
    d.reason = up ? "utilisation above target" : "utilisation below target";
    return d;
}

// Utilisation from two consecutive scrapes of a monotonically increasing busy time
// counter. This is how a scraper derives a rate, and it is the reason the worker exposes
// busy seconds as a counter rather than exposing a ratio it computed itself: a ratio
// computed inside the worker would carry the worker's own window, not the controller's.
inline double utilisation_from_delta(double busy_seconds_delta, double wall_seconds_delta,
                                     int worker_threads_total) {
    if (wall_seconds_delta <= 0.0 || worker_threads_total <= 0) return 0.0;
    const double capacity = wall_seconds_delta * static_cast<double>(worker_threads_total);
    if (capacity <= 0.0) return 0.0;
    return busy_seconds_delta / capacity;
}

}  // namespace stratus::scaling

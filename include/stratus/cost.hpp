// Cost per unit of work.
//
// Every price is an INPUT. Not one number in this file is a published cloud price, and
// none may ever be added: prices differ by region, by commitment, by account and by month,
// so a price baked into source is a number that is wrong the day after it is written. The
// calculator refuses to run without the prices being supplied explicitly.
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace stratus::cost {

struct Inputs {
    // Compute
    double price_per_instance_hour = -1.0;  // currency units per instance hour
    double instances = -1.0;                // average instance count over the window
    // Optional components. Negative means "not supplied", and the component is reported as
    // excluded rather than silently treated as zero.
    double price_per_gb_month_storage = -1.0;
    double storage_gb = -1.0;
    double price_per_gb_egress = -1.0;
    double egress_gb = -1.0;
    // Observed performance
    double throughput_units_per_second = -1.0;  // units of work completed per second
    double window_seconds = 3600.0;
};

struct Component {
    std::string name;
    double amount = 0.0;
    bool supplied = false;
};

struct Result {
    bool ok = false;
    std::string error;
    std::vector<Component> components;
    double total_cost = 0.0;          // cost over the window
    double units_of_work = 0.0;       // units completed over the window
    double cost_per_unit = 0.0;
    double cost_per_million_units = 0.0;
};

inline Result compute(const Inputs& in) {
    Result r;
    if (in.price_per_instance_hour < 0.0)
        return r.error = "price-per-instance-hour not supplied", r;
    if (in.instances <= 0.0) return r.error = "instances must be greater than zero", r;
    if (in.throughput_units_per_second <= 0.0)
        return r.error = "throughput must be greater than zero", r;
    if (in.window_seconds <= 0.0) return r.error = "window must be greater than zero", r;

    const double hours = in.window_seconds / 3600.0;
    const double compute_cost = in.price_per_instance_hour * in.instances * hours;
    r.components.push_back({"compute", compute_cost, true});

    if (in.price_per_gb_month_storage >= 0.0 && in.storage_gb >= 0.0) {
        const double months = in.window_seconds / (730.0 * 3600.0);  // 730 h billing month
        r.components.push_back(
            {"storage", in.price_per_gb_month_storage * in.storage_gb * months, true});
    } else {
        r.components.push_back({"storage", 0.0, false});
    }

    if (in.price_per_gb_egress >= 0.0 && in.egress_gb >= 0.0) {
        r.components.push_back({"egress", in.price_per_gb_egress * in.egress_gb, true});
    } else {
        r.components.push_back({"egress", 0.0, false});
    }

    for (const auto& c : r.components) r.total_cost += c.amount;
    r.units_of_work = in.throughput_units_per_second * in.window_seconds;
    r.cost_per_unit = r.total_cost / r.units_of_work;
    r.cost_per_million_units = r.cost_per_unit * 1e6;
    r.ok = true;
    return r;
}

}  // namespace stratus::cost

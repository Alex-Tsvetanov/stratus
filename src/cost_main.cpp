// stratus-cost: cost per unit of work.
//
// Every price is a required input. The tool refuses to guess, and there is no default
// price anywhere in the source, because a price compiled into a program is a claim about
// a provider's price list on the day the line was written, and nothing keeps it true.
#include <cstdio>
#include <iostream>
#include <string>

#include "stratus/args.hpp"
#include "stratus/cost.hpp"

namespace {

void usage() {
    std::cout <<
        "stratus-cost --price-instance-hour X --instances N --throughput R [options]\n"
        "\n"
        "  --price-instance-hour X   price of one instance for one hour, in your currency\n"
        "  --instances N             average instance count over the measurement window\n"
        "  --throughput R            completed units of work per second, measured\n"
        "  --window S                measurement window in seconds (default 3600)\n"
        "  --price-storage-gb-month X, --storage-gb N   optional storage component\n"
        "  --price-egress-gb X, --egress-gb N           optional egress component\n"
        "  --currency NAME           label only, printed in the report (default unit)\n"
        "\n"
        "No price is built in. Take the figures from the provider's own price list for the\n"
        "region and the commitment you actually used, and record where they came from.\n";
}

}  // namespace

int main(int argc, char** argv) {
    stratus::args::Args a(argc, argv);
    if (argc == 1 || a.has("help")) {
        usage();
        return argc == 1 ? 1 : 0;
    }

    stratus::cost::Inputs in;
    in.price_per_instance_hour = a.num_or("price-instance-hour", -1.0);
    in.instances = a.num_or("instances", -1.0);
    in.throughput_units_per_second = a.num_or("throughput", -1.0);
    in.window_seconds = a.num_or("window", 3600.0);
    in.price_per_gb_month_storage = a.num_or("price-storage-gb-month", -1.0);
    in.storage_gb = a.num_or("storage-gb", -1.0);
    in.price_per_gb_egress = a.num_or("price-egress-gb", -1.0);
    in.egress_gb = a.num_or("egress-gb", -1.0);

    const std::string currency = a.str_or("currency", "unit");

    const auto r = stratus::cost::compute(in);
    if (!r.ok) {
        std::cerr << "stratus-cost: " << r.error << "\n\n";
        usage();
        return 1;
    }

    std::printf("\n=== cost per unit of work ===\n");
    std::printf("window                %.0f s\n", in.window_seconds);
    std::printf("instances             %.3f\n", in.instances);
    std::printf("throughput            %.4f units/s\n", in.throughput_units_per_second);
    std::printf("units of work         %.0f\n", r.units_of_work);
    std::printf("\n%-12s %16s %s\n", "component", "amount", "");
    for (const auto& c : r.components) {
        if (c.supplied) std::printf("%-12s %16.6f %s\n", c.name.c_str(), c.amount, currency.c_str());
        else std::printf("%-12s %16s (price not supplied, excluded)\n", c.name.c_str(), "-");
    }
    std::printf("%-12s %16.6f %s\n", "total", r.total_cost, currency.c_str());
    std::printf("\ncost per unit          %.9f %s\n", r.cost_per_unit, currency.c_str());
    std::printf("cost per 10^6 units    %.6f %s\n", r.cost_per_million_units, currency.c_str());
    std::printf("\n");
    return 0;
}

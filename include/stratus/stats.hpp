// Latency statistics for the load generator.
//
// Percentiles are computed from the full sample, not from histogram buckets: the sample
// fits in memory at the rates this project drives, and reporting a bucket boundary as if
// it were a measured percentile is exactly the kind of quiet rounding that makes two
// systems look identical when they are not.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace stratus::stats {

// Nearest rank percentile on an already sorted sample. q in [0, 1].
inline double percentile_sorted(const std::vector<double>& sorted, double q) {
    if (sorted.empty()) return 0.0;
    if (q <= 0.0) return sorted.front();
    if (q >= 1.0) return sorted.back();
    const double rank = q * static_cast<double>(sorted.size());
    std::size_t idx = static_cast<std::size_t>(std::ceil(rank));
    if (idx == 0) idx = 1;
    if (idx > sorted.size()) idx = sorted.size();
    return sorted[idx - 1];
}

inline double percentile(std::vector<double> sample, double q) {
    std::sort(sample.begin(), sample.end());
    return percentile_sorted(sample, q);
}

inline double mean(const std::vector<double>& v) {
    if (v.empty()) return 0.0;
    double s = 0.0;
    for (double x : v) s += x;
    return s / static_cast<double>(v.size());
}

}  // namespace stratus::stats

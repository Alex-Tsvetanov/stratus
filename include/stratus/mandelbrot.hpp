// The unit of work. A deterministic, CPU bound, allocation free kernel whose cost is
// known in advance from its parameters, which is what makes it usable as the denominator
// of a cost per unit of work figure.
//
// The kernel samples the Mandelbrot escape time function on a size x size grid over a
// fixed window of the complex plane, with a fixed iteration ceiling. It returns both a
// checksum (so a response can be verified and so the compiler cannot elide the loop) and
// the exact number of inner iterations performed (so throughput can be reported in real
// arithmetic operations rather than in requests, which say nothing without their size).
#pragma once

#include <cstdint>
#include <cstddef>

namespace stratus {

struct WorkResult {
    std::uint64_t checksum = 0;    // sum of per-pixel escape counts
    std::uint64_t iterations = 0;  // inner loop iterations actually executed
};

// Window of the complex plane. Fixed on purpose: moving it would change the amount of
// work for the same size and silently invalidate every earlier measurement.
inline constexpr double kReMin = -2.0;
inline constexpr double kReMax = 0.5;
inline constexpr double kImMin = -1.25;
inline constexpr double kImMax = 1.25;

// Escape time for one point, capped at max_iter. Returns the iteration count.
constexpr std::uint32_t escape_time(double cr, double ci, std::uint32_t max_iter) noexcept {
    double zr = 0.0;
    double zi = 0.0;
    std::uint32_t n = 0;
    while (n < max_iter) {
        const double zr2 = zr * zr;
        const double zi2 = zi * zi;
        if (zr2 + zi2 > 4.0) break;
        zi = 2.0 * zr * zi + ci;
        zr = zr2 - zi2 + cr;
        ++n;
    }
    return n;
}

// size x size samples, max_iter ceiling. Cost is O(size^2 * max_iter) in the worst case
// and strictly bounded by it, which is why iterations is reported rather than assumed.
inline WorkResult compute(std::uint32_t size, std::uint32_t max_iter) noexcept {
    WorkResult out;
    if (size == 0 || max_iter == 0) return out;

    const double dr = (kReMax - kReMin) / static_cast<double>(size);
    const double di = (kImMax - kImMin) / static_cast<double>(size);

    for (std::uint32_t y = 0; y < size; ++y) {
        const double ci = kImMin + (static_cast<double>(y) + 0.5) * di;
        for (std::uint32_t x = 0; x < size; ++x) {
            const double cr = kReMin + (static_cast<double>(x) + 0.5) * dr;
            const std::uint32_t n = escape_time(cr, ci, max_iter);
            out.checksum += n;
            out.iterations += n;
        }
    }
    return out;
}

}  // namespace stratus

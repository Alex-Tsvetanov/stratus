// Metric primitives and the Prometheus text exposition format.
//
// The format is the whole point of writing this by hand: the autoscaler in this project
// reads the same bytes a real scraper would, so the control loop is not coupled to an
// internal representation that only exists inside this process.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace stratus::metrics {

// Formats a double the way the exposition format wants it: plain decimal, no locale, no
// scientific notation for ordinary magnitudes, and the literal +Inf for the last bucket.
inline std::string fmt(double v) {
    std::ostringstream os;
    os.imbue(std::locale::classic());
    os.precision(6);
    os << std::fixed << v;
    std::string s = os.str();
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    return s.empty() ? "0" : s;
}

class Counter {
public:
    void inc(double v = 1.0) noexcept {
        // relaxed is enough: the exposition is a snapshot, not a synchronisation point.
        double cur = value_.load(std::memory_order_relaxed);
        while (!value_.compare_exchange_weak(cur, cur + v, std::memory_order_relaxed)) {}
    }
    double get() const noexcept { return value_.load(std::memory_order_relaxed); }

private:
    std::atomic<double> value_{0.0};
};

class Gauge {
public:
    void set(double v) noexcept { value_.store(v, std::memory_order_relaxed); }
    void add(double v) noexcept {
        double cur = value_.load(std::memory_order_relaxed);
        while (!value_.compare_exchange_weak(cur, cur + v, std::memory_order_relaxed)) {}
    }
    double get() const noexcept { return value_.load(std::memory_order_relaxed); }

private:
    std::atomic<double> value_{0.0};
};

// Fixed bucket histogram. Buckets are cumulative on exposition, as the format requires.
class Histogram {
public:
    explicit Histogram(std::vector<double> upper_bounds)
        : bounds_(std::move(upper_bounds)), counts_(bounds_.size() + 1, 0) {}

    void observe(double v) {
        std::lock_guard<std::mutex> lock(mu_);
        std::size_t i = 0;
        while (i < bounds_.size() && v > bounds_[i]) ++i;
        ++counts_[i];
        sum_ += v;
        ++count_;
    }

    // Cumulative counts, one per bound, plus the +Inf bucket at the end.
    std::vector<std::uint64_t> cumulative() const {
        std::lock_guard<std::mutex> lock(mu_);
        std::vector<std::uint64_t> out(counts_.size(), 0);
        std::uint64_t acc = 0;
        for (std::size_t i = 0; i < counts_.size(); ++i) {
            acc += counts_[i];
            out[i] = acc;
        }
        return out;
    }

    const std::vector<double>& bounds() const noexcept { return bounds_; }
    double sum() const {
        std::lock_guard<std::mutex> lock(mu_);
        return sum_;
    }
    std::uint64_t count() const {
        std::lock_guard<std::mutex> lock(mu_);
        return count_;
    }

private:
    mutable std::mutex mu_;
    std::vector<double> bounds_;
    std::vector<std::uint64_t> counts_;
    double sum_ = 0.0;
    std::uint64_t count_ = 0;
};

using Labels = std::vector<std::pair<std::string, std::string>>;

inline std::string render_labels(const Labels& labels) {
    if (labels.empty()) return {};
    std::string out = "{";
    for (std::size_t i = 0; i < labels.size(); ++i) {
        if (i) out += ',';
        out += labels[i].first;
        out += "=\"";
        for (char c : labels[i].second) {
            if (c == '\\' || c == '"') out += '\\';
            if (c == '\n') { out += "\\n"; continue; }
            out += c;
        }
        out += '"';
    }
    out += '}';
    return out;
}

// Builds an exposition document. Kept as a plain string builder rather than a registry of
// registered objects: the worker has a fixed, small set of metrics, and a registry would
// be indirection with exactly one user.
class Exposition {
public:
    void counter(std::string_view name, std::string_view help, double value,
                 const Labels& labels = {}) {
        header(name, help, "counter");
        line(name, labels, value);
    }

    void gauge(std::string_view name, std::string_view help, double value,
               const Labels& labels = {}) {
        header(name, help, "gauge");
        line(name, labels, value);
    }

    void histogram(std::string_view name, std::string_view help, const Histogram& h,
                   const Labels& labels = {}) {
        header(name, help, "histogram");
        const auto cum = h.cumulative();
        const auto& bounds = h.bounds();
        for (std::size_t i = 0; i < bounds.size(); ++i) {
            Labels l = labels;
            l.emplace_back("le", fmt(bounds[i]));
            out_ += std::string(name) + "_bucket" + render_labels(l) + " " +
                    std::to_string(cum[i]) + "\n";
        }
        Labels inf = labels;
        inf.emplace_back("le", "+Inf");
        out_ += std::string(name) + "_bucket" + render_labels(inf) + " " +
                std::to_string(cum.back()) + "\n";
        out_ += std::string(name) + "_sum" + render_labels(labels) + " " + fmt(h.sum()) + "\n";
        out_ += std::string(name) + "_count" + render_labels(labels) + " " +
                std::to_string(h.count()) + "\n";
    }

    void raw(std::string_view text) { out_ += text; }

    const std::string& str() const noexcept { return out_; }

private:
    // HELP and TYPE appear once per metric name, never once per series. A repeated header
    // makes the whole document invalid for a scraper, and a metric with several label sets
    // (requests by endpoint and status, for instance) would otherwise emit one per series.
    void header(std::string_view name, std::string_view help, std::string_view type) {
        const std::string key(name);
        for (const auto& seen : declared_) {
            if (seen == key) return;
        }
        declared_.push_back(key);
        out_ += "# HELP " + key + " " + std::string(help) + "\n";
        out_ += "# TYPE " + key + " " + std::string(type) + "\n";
    }
    void line(std::string_view name, const Labels& labels, double value) {
        out_ += std::string(name) + render_labels(labels) + " " + fmt(value) + "\n";
    }

    std::string out_;
    std::vector<std::string> declared_;
};

}  // namespace stratus::metrics

// Reading the Prometheus text exposition format back.
//
// The proxy aggregates the fleet by summing samples scraped from every worker, and the
// autoscaler derives utilisation from two consecutive scrapes. Both need to read the same
// text a real scraper reads, so the parser lives here and is shared by both.
#pragma once

#include <charconv>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace stratus::prom {

struct Sample {
    std::string name;    // metric name, label set included, verbatim as exposed
    double value = 0.0;
};

inline std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

inline bool parse_double(std::string_view s, double& out) {
    if (s == "+Inf") { out = 1e308; return true; }
    if (s == "-Inf") { out = -1e308; return true; }
    if (s == "NaN") return false;
    const auto res = std::from_chars(s.data(), s.data() + s.size(), out);
    return res.ec == std::errc{} && res.ptr == s.data() + s.size();
}

// One sample per non comment line. The series key keeps the label set attached, because
// two series of the same metric with different labels must not be merged.
inline std::vector<Sample> parse(std::string_view text) {
    std::vector<Sample> out;
    while (!text.empty()) {
        const auto nl = text.find('\n');
        std::string_view line = trim(text.substr(0, nl));
        text = (nl == std::string_view::npos) ? std::string_view{} : text.substr(nl + 1);
        if (line.empty() || line.front() == '#') continue;

        // The value is whatever follows the last space; label values may contain spaces.
        const auto sp = line.rfind(' ');
        if (sp == std::string_view::npos) continue;
        double v = 0.0;
        if (!parse_double(trim(line.substr(sp + 1)), v)) continue;
        out.push_back(Sample{std::string(trim(line.substr(0, sp))), v});
    }
    return out;
}

// HELP and TYPE lines, keyed by metric name. The aggregating proxy has to re-emit them,
// and inventing its own text would make the fleet document disagree with the per worker
// documents it was built from.
struct Meta {
    std::string help;
    std::string type;
};

inline std::map<std::string, Meta> parse_meta(std::string_view text) {
    std::map<std::string, Meta> out;
    while (!text.empty()) {
        const auto nl = text.find('\n');
        std::string_view line = trim(text.substr(0, nl));
        text = (nl == std::string_view::npos) ? std::string_view{} : text.substr(nl + 1);
        if (line.size() < 8 || line.front() != '#') continue;
        const bool is_help = line.substr(0, 7) == "# HELP ";
        const bool is_type = line.substr(0, 7) == "# TYPE ";
        if (!is_help && !is_type) continue;
        std::string_view rest = line.substr(7);
        const auto sp = rest.find(' ');
        if (sp == std::string_view::npos) continue;
        const std::string name(rest.substr(0, sp));
        if (is_help) out[name].help = std::string(trim(rest.substr(sp + 1)));
        else out[name].type = std::string(trim(rest.substr(sp + 1)));
    }
    return out;
}

// Metric name of a series key, that is everything before the label brace.
inline std::string metric_name(std::string_view series) {
    const auto brace = series.find('{');
    std::string name(brace == std::string_view::npos ? series : series.substr(0, brace));
    // Histogram child series share the HELP and TYPE of their parent metric.
    for (std::string_view suffix : {"_bucket", "_sum", "_count"}) {
        if (name.size() > suffix.size() &&
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
            return name.substr(0, name.size() - suffix.size());
        }
    }
    return name;
}

// Removes one label from a series key. Used to fold per instance series into fleet totals:
// keeping the instance label would produce one series per replica, and summing them is the
// whole point of the aggregation.
inline std::string strip_label(std::string_view series, std::string_view label) {
    const auto open = series.find('{');
    if (open == std::string_view::npos) return std::string(series);
    const auto close = series.rfind('}');
    if (close == std::string_view::npos || close < open) return std::string(series);

    std::string kept;
    std::string_view inner = series.substr(open + 1, close - open - 1);
    const std::string prefix = std::string(label) + "=";
    while (!inner.empty()) {
        // Label values are quoted and may contain commas, so scan for the separator
        // outside the quotes rather than splitting on every comma.
        std::size_t i = 0;
        bool in_quotes = false;
        for (; i < inner.size(); ++i) {
            if (inner[i] == '"' && (i == 0 || inner[i - 1] != '\\')) in_quotes = !in_quotes;
            else if (inner[i] == ',' && !in_quotes) break;
        }
        std::string_view pair = inner.substr(0, i);
        inner = (i >= inner.size()) ? std::string_view{} : inner.substr(i + 1);
        if (pair.size() >= prefix.size() && pair.compare(0, prefix.size(), prefix) == 0) continue;
        if (!kept.empty()) kept += ',';
        kept += std::string(pair);
    }
    std::string out(series.substr(0, open));
    if (!kept.empty()) out += "{" + kept + "}";
    return out;
}

// Sums matching series across several scraped documents. Counters and histogram buckets
// add correctly under this rule; gauges add to a fleet total, which is what the autoscaler
// wants for in flight requests and worker threads.
inline std::map<std::string, double> aggregate(const std::vector<std::string>& documents) {
    std::map<std::string, double> total;
    for (const auto& doc : documents) {
        for (const auto& s : parse(doc)) total[s.name] += s.value;
    }
    return total;
}

// Looks a series up by exact key, or by metric name when the caller does not care about
// labels and the metric has exactly one series.
inline bool find(const std::map<std::string, double>& m, std::string_view key, double& out) {
    const auto it = m.find(std::string(key));
    if (it == m.end()) return false;
    out = it->second;
    return true;
}

}  // namespace stratus::prom

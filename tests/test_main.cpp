// Unit tests for the logic that decides something.
//
// Getters are not tested. What is tested is the compute kernel's determinism and cost
// bound, the HTTP surface the service actually parses, the exposition format the
// controller actually reads, the scaling policy, the cost model and the statistics used
// to report latency. Everything here runs without a socket and without a container, which
// is the reason the interesting parts were separated from the mains in the first place.
#include "test_framework.hpp"

#include <string>
#include <vector>

#include "stratus/cost.hpp"
#include "stratus/http.hpp"
#include "stratus/mandelbrot.hpp"
#include "stratus/metrics.hpp"
#include "stratus/net.hpp"
#include "stratus/promparse.hpp"
#include "stratus/scaling.hpp"
#include "stratus/stats.hpp"

// ---------------------------------------------------------------------------
// The compute kernel
// ---------------------------------------------------------------------------

TEST(mandelbrot_deterministic) {
    const auto a = stratus::compute(64, 120);
    const auto b = stratus::compute(64, 120);
    CHECK_EQ(a.checksum, b.checksum);
    CHECK_EQ(a.iterations, b.iterations);
    CHECK(a.iterations > 0);

    // A single sample lands at the centre of the window, the point -0.75 + 0i, which
    // belongs to the set and therefore uses the whole iteration budget.
    const auto one = stratus::compute(1, 10);
    CHECK_EQ(one.checksum, static_cast<std::uint64_t>(10));
    CHECK_EQ(one.iterations, static_cast<std::uint64_t>(10));
}

TEST(mandelbrot_interior_point_saturates) {
    CHECK_EQ(stratus::escape_time(0.0, 0.0, 1000), 1000u);
    CHECK_EQ(stratus::escape_time(-1.0, 0.0, 1000), 1000u);
    // Far outside: one iteration takes the orbit past the escape radius.
    CHECK_EQ(stratus::escape_time(2.0, 2.0, 100), 1u);
    CHECK_EQ(stratus::escape_time(0.0, 0.0, 0), 0u);
}

TEST(mandelbrot_iteration_budget) {
    const std::uint32_t size = 32, iter = 50;
    const auto r = stratus::compute(size, iter);
    const std::uint64_t bound =
        static_cast<std::uint64_t>(size) * size * iter;
    CHECK(r.iterations > 0);
    CHECK(r.iterations <= bound);

    // Degenerate parameters do no work rather than looping on a zero sized grid.
    CHECK_EQ(stratus::compute(0, 100).iterations, static_cast<std::uint64_t>(0));
    CHECK_EQ(stratus::compute(100, 0).iterations, static_cast<std::uint64_t>(0));
}

TEST(mandelbrot_size_scaling) {
    const auto small = stratus::compute(32, 80);
    const auto large = stratus::compute(64, 80);
    CHECK(large.iterations > small.iterations);

    // Raising the ceiling can only add iterations, never remove them.
    const auto deeper = stratus::compute(32, 160);
    CHECK(deeper.iterations >= small.iterations);
}

// ---------------------------------------------------------------------------
// HTTP
// ---------------------------------------------------------------------------

TEST(http_request_line_parsing) {
    const std::string raw =
        "GET /work?size=128&iter=300 HTTP/1.1\r\n"
        "Host: localhost:8080\r\n"
        "User-Agent: stratus-loadgen\r\n"
        "\r\n";
    const auto req = stratus::http::parse_request(raw);
    CHECK(req.has_value());
    CHECK_EQ(req->method, std::string("GET"));
    CHECK_EQ(req->path, std::string("/work"));
    CHECK_EQ(req->target, std::string("/work?size=128&iter=300"));
    CHECK_EQ(req->headers.at("host"), std::string("localhost:8080"));
    CHECK_EQ(req->headers.at("user-agent"), std::string("stratus-loadgen"));

    // An incomplete head yields nothing, which is how the read loop knows to read on.
    CHECK(!stratus::http::parse_request("GET /work HTTP/1.1\r\nHost: x\r\n").has_value());
}

TEST(http_query_parameters) {
    const auto req = stratus::http::parse_request(
        "GET /work?size=512&iter=1000&tag=a%20b&flag HTTP/1.1\r\n\r\n");
    CHECK(req.has_value());
    CHECK_EQ(req->query.at("size"), std::string("512"));
    CHECK_EQ(req->query.at("iter"), std::string("1000"));
    CHECK_EQ(req->query.at("tag"), std::string("a b"));
    CHECK_EQ(req->query.at("flag"), std::string(""));
    CHECK_EQ(stratus::http::int_param(*req, "size", 1, 1, 4096).value(), 512LL);
}

TEST(http_missing_query_parameter) {
    const auto req = stratus::http::parse_request("GET /work HTTP/1.1\r\n\r\n");
    CHECK(req.has_value());
    CHECK(req->query.empty());
    const auto v = stratus::http::int_param(*req, "size", 256, 1, 4096);
    CHECK(v.has_value());
    CHECK_EQ(*v, 256LL);
}

TEST(http_bad_query_parameter) {
    const auto req = stratus::http::parse_request(
        "GET /work?size=abc&iter=999999&trail=12x HTTP/1.1\r\n\r\n");
    CHECK(req.has_value());
    // Not a number at all.
    CHECK(!stratus::http::int_param(*req, "size", 256, 1, 4096).has_value());
    // A number, but outside the accepted range: answered with 400, not clamped, because
    // silently substituting a different size would falsify every measurement made with it.
    CHECK(!stratus::http::int_param(*req, "iter", 400, 1, 100000).has_value());
    // Trailing rubbish after a valid prefix must not be accepted as 12.
    CHECK(!stratus::http::int_param(*req, "trail", 0, 0, 1000).has_value());
}

TEST(http_response_serialisation) {
    stratus::http::Response r;
    r.status = 400;
    r.reason = "Bad Request";
    r.body = "nope\n";
    const std::string wire = stratus::http::serialise(r);
    CHECK(wire.rfind("HTTP/1.1 400 Bad Request\r\n", 0) == 0);
    CHECK(wire.find("Content-Length: 5\r\n") != std::string::npos);

    const auto parsed = stratus::http::parse_response(wire);
    CHECK_EQ(parsed.status, 400);
    CHECK_EQ(parsed.body, std::string("nope\n"));
}

// ---------------------------------------------------------------------------
// Metric exposition
// ---------------------------------------------------------------------------

TEST(metrics_counter_exposition) {
    stratus::metrics::Counter c;
    c.inc();
    c.inc(2.5);
    CHECK_NEAR(c.get(), 3.5, 1e-9);

    stratus::metrics::Exposition e;
    e.counter("stratus_requests_total", "Requests served", c.get(),
              {{"endpoint", "work"}, {"status", "200"}});
    const std::string s = e.str();
    CHECK(s.find("# HELP stratus_requests_total Requests served\n") != std::string::npos);
    CHECK(s.find("# TYPE stratus_requests_total counter\n") != std::string::npos);
    CHECK(s.find("stratus_requests_total{endpoint=\"work\",status=\"200\"} 3.5\n") !=
          std::string::npos);

    // A second series of the same metric must not repeat the header: a scraper rejects a
    // document that declares the same metric twice, and the four label sets the worker
    // exposes for this counter would each have carried one.
    e.counter("stratus_requests_total", "Requests served", 1.0,
              {{"endpoint", "healthz"}, {"status", "200"}});
    const std::string two = e.str();
    CHECK_EQ(two.find("# TYPE stratus_requests_total counter"),
             two.rfind("# TYPE stratus_requests_total counter"));
}

TEST(metrics_gauge_and_labels) {
    stratus::metrics::Gauge g;
    g.set(4.0);
    g.add(-1.0);
    CHECK_NEAR(g.get(), 3.0, 1e-9);

    stratus::metrics::Exposition e;
    e.gauge("stratus_inflight_requests", "In flight", g.get(), {{"instance", "w\"1"}});
    // A quote inside a label value must be escaped, or the document stops being parseable
    // at that line and every series after it is lost.
    CHECK(e.str().find("{instance=\"w\\\"1\"} 3\n") != std::string::npos);
}

TEST(metrics_histogram_buckets) {
    stratus::metrics::Histogram h({0.1, 0.5, 1.0});
    h.observe(0.05);
    h.observe(0.2);
    h.observe(0.2);
    h.observe(2.0);

    const auto cum = h.cumulative();
    CHECK_EQ(cum.size(), static_cast<std::size_t>(4));
    CHECK_EQ(cum[0], static_cast<std::uint64_t>(1));
    CHECK_EQ(cum[1], static_cast<std::uint64_t>(3));
    CHECK_EQ(cum[2], static_cast<std::uint64_t>(3));
    CHECK_EQ(cum[3], static_cast<std::uint64_t>(4));
    CHECK_EQ(h.count(), static_cast<std::uint64_t>(4));
    CHECK_NEAR(h.sum(), 2.45, 1e-9);

    stratus::metrics::Exposition e;
    e.histogram("stratus_work_duration_seconds", "Work time", h);
    const std::string s = e.str();
    CHECK(s.find("stratus_work_duration_seconds_bucket{le=\"0.1\"} 1\n") != std::string::npos);
    CHECK(s.find("stratus_work_duration_seconds_bucket{le=\"+Inf\"} 4\n") != std::string::npos);
    CHECK(s.find("stratus_work_duration_seconds_count 4\n") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Reading the exposition format back
// ---------------------------------------------------------------------------

TEST(prom_parse_counter_with_labels) {
    const std::string doc =
        "# HELP stratus_requests_total Requests served\n"
        "# TYPE stratus_requests_total counter\n"
        "stratus_requests_total{instance=\"w1\",endpoint=\"work\",status=\"200\"} 42\n"
        "stratus_busy_seconds_total{instance=\"w1\"} 12.5\n";
    const auto samples = stratus::prom::parse(doc);
    CHECK_EQ(samples.size(), static_cast<std::size_t>(2));
    CHECK_EQ(samples[0].name,
             std::string("stratus_requests_total{instance=\"w1\",endpoint=\"work\",status=\"200\"}"));
    CHECK_NEAR(samples[0].value, 42.0, 1e-9);
    CHECK_NEAR(samples[1].value, 12.5, 1e-9);

    CHECK_EQ(stratus::prom::metric_name(samples[0].name), std::string("stratus_requests_total"));
    CHECK_EQ(stratus::prom::metric_name("stratus_work_duration_seconds_bucket{le=\"0.1\"}"),
             std::string("stratus_work_duration_seconds"));
}

TEST(prom_parse_skips_comments) {
    const std::string doc =
        "# HELP m_total A metric\n"
        "# TYPE m_total counter\n"
        "\n"
        "m_total 7\n"
        "# a stray comment\n"
        "garbage_without_value\n";
    const auto samples = stratus::prom::parse(doc);
    CHECK_EQ(samples.size(), static_cast<std::size_t>(1));
    CHECK_EQ(samples[0].name, std::string("m_total"));

    const auto meta = stratus::prom::parse_meta(doc);
    CHECK_EQ(meta.at("m_total").help, std::string("A metric"));
    CHECK_EQ(meta.at("m_total").type, std::string("counter"));
}

TEST(prom_aggregate_across_targets) {
    const std::string w1 =
        "stratus_busy_seconds_total{instance=\"w1\"} 10\n"
        "stratus_worker_threads{instance=\"w1\"} 4\n";
    const std::string w2 =
        "stratus_busy_seconds_total{instance=\"w2\"} 6\n"
        "stratus_worker_threads{instance=\"w2\"} 4\n";

    // Without folding the identity away, two replicas produce two series.
    const auto raw = stratus::prom::aggregate({w1, w2});
    CHECK_EQ(raw.size(), static_cast<std::size_t>(4));

    // Folding it away is what makes the fleet total appear.
    CHECK_EQ(stratus::prom::strip_label("stratus_busy_seconds_total{instance=\"w1\"}", "instance"),
             std::string("stratus_busy_seconds_total"));
    CHECK_EQ(stratus::prom::strip_label(
                 "stratus_requests_total{instance=\"w1\",endpoint=\"work\"}", "instance"),
             std::string("stratus_requests_total{endpoint=\"work\"}"));

    std::map<std::string, double> fleet;
    for (const auto& doc : {w1, w2})
        for (const auto& s : stratus::prom::parse(doc))
            fleet[stratus::prom::strip_label(s.name, "instance")] += s.value;
    CHECK_NEAR(fleet.at("stratus_busy_seconds_total"), 16.0, 1e-9);
    CHECK_NEAR(fleet.at("stratus_worker_threads"), 8.0, 1e-9);
}

// ---------------------------------------------------------------------------
// The scaling policy
// ---------------------------------------------------------------------------

TEST(scaling_scales_up_above_target) {
    stratus::scaling::Policy p;  // target 0.70
    const auto d = stratus::scaling::decide(p, 2, 0.95, 10.0);
    CHECK(d.action == stratus::scaling::Action::ScaleUp);
    CHECK_EQ(d.desired_replicas, 3);
    CHECK_NEAR(d.raw_desired, 2.0 * 0.95 / 0.70, 1e-9);
}

TEST(scaling_respects_cooldown) {
    stratus::scaling::Policy p;
    p.scale_up_cooldown_s = 5.0;
    const auto d = stratus::scaling::decide(p, 2, 0.95, 1.0);
    CHECK(d.action == stratus::scaling::Action::Hold);
    CHECK_EQ(d.desired_replicas, 2);
    CHECK(d.reason.find("cooldown") != std::string::npos);
}

TEST(scaling_clamps_to_bounds) {
    stratus::scaling::Policy p;
    p.min_replicas = 2;
    p.max_replicas = 4;

    const auto up = stratus::scaling::decide(p, 3, 2.0, 100.0);
    CHECK(up.action == stratus::scaling::Action::ScaleUp);
    CHECK_EQ(up.desired_replicas, 4);  // raw desire is far above the ceiling

    const auto down = stratus::scaling::decide(p, 3, 0.05, 100.0);
    CHECK(down.action == stratus::scaling::Action::ScaleDown);
    CHECK_EQ(down.desired_replicas, 2);  // raw desire is below the floor
}

TEST(scaling_dead_band_holds) {
    stratus::scaling::Policy p;  // target 0.70, dead band 0.10
    // Four replicas at 0.72 want 4.11, which rounds up to five. Acting on that would add
    // a replica for a 3 per cent overshoot and remove it again on the next reading.
    const auto d = stratus::scaling::decide(p, 4, 0.72, 100.0);
    CHECK(d.action == stratus::scaling::Action::Hold);
    CHECK_EQ(d.reason, std::string("within dead band"));
}

TEST(scaling_scale_down_needs_longer_calm) {
    stratus::scaling::Policy p;
    p.scale_up_cooldown_s = 5.0;
    p.scale_down_cooldown_s = 20.0;

    const auto early = stratus::scaling::decide(p, 4, 0.20, 10.0);
    CHECK(early.action == stratus::scaling::Action::Hold);
    CHECK(early.reason.find("scale-down") != std::string::npos);

    const auto later = stratus::scaling::decide(p, 4, 0.20, 30.0);
    CHECK(later.action == stratus::scaling::Action::ScaleDown);
    CHECK_EQ(later.desired_replicas, 2);
}

TEST(utilisation_from_counter_delta) {
    // Six busy seconds accumulated across a fleet of four threads over two wall seconds
    // is seventy five per cent of capacity.
    CHECK_NEAR(stratus::scaling::utilisation_from_delta(6.0, 2.0, 4), 0.75, 1e-9);
    // Degenerate readings must not produce an infinite utilisation and a runaway scale up.
    CHECK_NEAR(stratus::scaling::utilisation_from_delta(6.0, 0.0, 4), 0.0, 1e-9);
    CHECK_NEAR(stratus::scaling::utilisation_from_delta(6.0, 2.0, 0), 0.0, 1e-9);
}

// ---------------------------------------------------------------------------
// The cost model
// ---------------------------------------------------------------------------

TEST(cost_per_unit_of_work) {
    stratus::cost::Inputs in;
    in.price_per_instance_hour = 0.05;  // an input, not a published price
    in.instances = 2.0;
    in.throughput_units_per_second = 10.0;
    in.window_seconds = 3600.0;

    const auto r = stratus::cost::compute(in);
    CHECK(r.ok);
    CHECK_NEAR(r.total_cost, 0.10, 1e-12);
    CHECK_NEAR(r.units_of_work, 36000.0, 1e-6);
    CHECK_NEAR(r.cost_per_unit, 0.10 / 36000.0, 1e-15);
    CHECK_NEAR(r.cost_per_million_units, 0.10 / 36000.0 * 1e6, 1e-9);

    // Storage is only included when both its price and its size are given.
    in.price_per_gb_month_storage = 0.02;
    in.storage_gb = 5.0;
    const auto with_storage = stratus::cost::compute(in);
    CHECK(with_storage.ok);
    CHECK_NEAR(with_storage.total_cost, 0.10 + 0.02 * 5.0 / 730.0, 1e-12);
}

TEST(cost_rejects_unpriced_input) {
    stratus::cost::Inputs missing_price;
    missing_price.instances = 1.0;
    missing_price.throughput_units_per_second = 1.0;
    const auto a = stratus::cost::compute(missing_price);
    CHECK(!a.ok);
    CHECK(!a.error.empty());

    stratus::cost::Inputs no_throughput;
    no_throughput.price_per_instance_hour = 0.05;
    no_throughput.instances = 1.0;
    no_throughput.throughput_units_per_second = 0.0;
    const auto b = stratus::cost::compute(no_throughput);
    CHECK(!b.ok);

    // A supplied storage price without a supplied size is reported as excluded rather
    // than contributing zero as if it had been measured.
    stratus::cost::Inputs half_storage;
    half_storage.price_per_instance_hour = 0.05;
    half_storage.instances = 1.0;
    half_storage.throughput_units_per_second = 1.0;
    half_storage.price_per_gb_month_storage = 0.02;
    const auto c = stratus::cost::compute(half_storage);
    CHECK(c.ok);
    for (const auto& comp : c.components)
        if (comp.name == "storage") CHECK(!comp.supplied);
}

// ---------------------------------------------------------------------------
// Latency statistics
// ---------------------------------------------------------------------------

TEST(stats_percentiles) {
    const std::vector<double> v{5, 3, 9, 1, 7, 2, 8, 4, 10, 6};
    CHECK_NEAR(stratus::stats::percentile(v, 0.50), 5.0, 1e-9);
    CHECK_NEAR(stratus::stats::percentile(v, 0.90), 9.0, 1e-9);
    CHECK_NEAR(stratus::stats::percentile(v, 0.99), 10.0, 1e-9);
    CHECK_NEAR(stratus::stats::percentile(v, 0.0), 1.0, 1e-9);
    CHECK_NEAR(stratus::stats::percentile(v, 1.0), 10.0, 1e-9);
    CHECK_NEAR(stratus::stats::mean(v), 5.5, 1e-9);
}

TEST(stats_empty_sample) {
    const std::vector<double> empty;
    CHECK_NEAR(stratus::stats::percentile(empty, 0.95), 0.0, 1e-9);
    CHECK_NEAR(stratus::stats::mean(empty), 0.0, 1e-9);

    const std::vector<double> one{2.5};
    CHECK_NEAR(stratus::stats::percentile(one, 0.5), 2.5, 1e-9);
    CHECK_NEAR(stratus::stats::percentile(one, 0.99), 2.5, 1e-9);
}

// ---------------------------------------------------------------------------
// Endpoint list parsing
// ---------------------------------------------------------------------------

TEST(backend_list_parsing) {
    const auto eps = stratus::net::parse_endpoints(
        "10.0.0.1:9001, worker , 10.0.0.2:80 ,,bad:0,too-big:99999", 8081);
    CHECK_EQ(eps.size(), static_cast<std::size_t>(3));
    CHECK_EQ(eps[0].host, std::string("10.0.0.1"));
    CHECK_EQ(eps[0].port, static_cast<std::uint16_t>(9001));
    CHECK_EQ(eps[1].host, std::string("worker"));
    CHECK_EQ(eps[1].port, static_cast<std::uint16_t>(8081));  // default applied
    CHECK_EQ(eps[2].port, static_cast<std::uint16_t>(80));

    CHECK(stratus::net::parse_endpoints("", 8081).empty());
}

int main(int argc, char** argv) { return testing::run(argc, argv); }

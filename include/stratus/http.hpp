// Minimal HTTP/1.1 message handling. Parsing is pure and header only so the tests can
// exercise it without opening a socket; the socket work lives in net.hpp / httpd.hpp.
//
// Scope is deliberately small: the request line, the headers we actually read, and a
// response serialiser. There is no chunked transfer encoding, no keep alive pipelining
// and no multipart handling, because the service speaks to exactly two clients, both in
// this repository, and neither of them needs any of it.
#pragma once

#include <algorithm>
#include <cctype>
#include <charconv>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace stratus::http {

struct Request {
    std::string method;
    std::string target;  // full request target, query string included
    std::string path;    // target up to the '?'
    std::string body;
    std::unordered_map<std::string, std::string> query;
    std::unordered_map<std::string, std::string> headers;  // keys lowercased
};

struct Response {
    int status = 200;
    std::string reason = "OK";
    std::string content_type = "text/plain; charset=utf-8";
    std::string body;
};

inline std::string to_lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

inline std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

// Percent decoding. Malformed escapes are left as written rather than rejected: the
// query string is a tuning knob, not a trust boundary, and a stricter reading would only
// turn a typo into a connection error.
inline std::string percent_decode(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') {
            out.push_back(' ');
        } else if (s[i] == '%' && i + 2 < s.size() && hex(s[i + 1]) >= 0 && hex(s[i + 2]) >= 0) {
            out.push_back(static_cast<char>(hex(s[i + 1]) * 16 + hex(s[i + 2])));
            i += 2;
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

inline std::unordered_map<std::string, std::string> parse_query(std::string_view qs) {
    std::unordered_map<std::string, std::string> out;
    while (!qs.empty()) {
        const auto amp = qs.find('&');
        std::string_view pair = qs.substr(0, amp);
        qs = (amp == std::string_view::npos) ? std::string_view{} : qs.substr(amp + 1);
        if (pair.empty()) continue;
        const auto eq = pair.find('=');
        if (eq == std::string_view::npos) {
            out.emplace(percent_decode(pair), std::string{});
        } else {
            out.insert_or_assign(percent_decode(pair.substr(0, eq)),
                                 percent_decode(pair.substr(eq + 1)));
        }
    }
    return out;
}

// Parses the head of a request. Returns nothing when the buffer does not yet hold a
// complete head, which is how the read loop knows to keep reading.
inline std::optional<Request> parse_request(std::string_view raw) {
    const auto head_end = raw.find("\r\n\r\n");
    if (head_end == std::string_view::npos) return std::nullopt;

    Request req;
    std::string_view head = raw.substr(0, head_end);

    const auto line_end = head.find("\r\n");
    std::string_view line = head.substr(0, line_end);
    const auto sp1 = line.find(' ');
    if (sp1 == std::string_view::npos) return std::nullopt;
    const auto sp2 = line.find(' ', sp1 + 1);
    if (sp2 == std::string_view::npos) return std::nullopt;

    req.method = std::string(line.substr(0, sp1));
    req.target = std::string(line.substr(sp1 + 1, sp2 - sp1 - 1));

    const auto qmark = req.target.find('?');
    if (qmark == std::string::npos) {
        req.path = req.target;
    } else {
        req.path = req.target.substr(0, qmark);
        req.query = parse_query(std::string_view(req.target).substr(qmark + 1));
    }

    std::string_view rest = (line_end == std::string_view::npos)
                                ? std::string_view{}
                                : head.substr(line_end + 2);
    while (!rest.empty()) {
        const auto nl = rest.find("\r\n");
        std::string_view hline = rest.substr(0, nl);
        rest = (nl == std::string_view::npos) ? std::string_view{} : rest.substr(nl + 2);
        const auto colon = hline.find(':');
        if (colon == std::string_view::npos) continue;
        req.headers.insert_or_assign(to_lower(trim(hline.substr(0, colon))),
                                     std::string(trim(hline.substr(colon + 1))));
    }

    req.body = std::string(raw.substr(head_end + 4));
    return req;
}

// Query parameter as an integer, with a default and an inclusive range. Returns nothing
// when the parameter is present but not a number in range, so the caller can answer 400
// instead of silently substituting a value the client did not ask for.
inline std::optional<long long> int_param(const Request& req, std::string_view name,
                                          long long fallback, long long lo, long long hi) {
    const auto it = req.query.find(std::string(name));
    if (it == req.query.end()) return fallback;
    const std::string& v = it->second;
    long long parsed = 0;
    const char* first = v.data();
    const char* last = v.data() + v.size();
    const auto res = std::from_chars(first, last, parsed);
    if (res.ec != std::errc{} || res.ptr != last) return std::nullopt;
    if (parsed < lo || parsed > hi) return std::nullopt;
    return parsed;
}

inline std::string serialise(const Response& r) {
    std::string out;
    out.reserve(r.body.size() + 160);
    out += "HTTP/1.1 ";
    out += std::to_string(r.status);
    out += ' ';
    out += r.reason;
    out += "\r\nContent-Type: ";
    out += r.content_type;
    out += "\r\nContent-Length: ";
    out += std::to_string(r.body.size());
    out += "\r\nConnection: close\r\n\r\n";
    out += r.body;
    return out;
}

// Splits an HTTP response into status code and body. Used by the proxy, the autoscaler
// and the load generator, all of which are clients of the worker.
struct ClientResponse {
    int status = 0;
    std::string body;
};

inline ClientResponse parse_response(std::string_view raw) {
    ClientResponse out;
    const auto sp1 = raw.find(' ');
    if (sp1 == std::string_view::npos) return out;
    const auto sp2 = raw.find(' ', sp1 + 1);
    const auto code_end = (sp2 == std::string_view::npos) ? raw.find("\r\n", sp1 + 1) : sp2;
    if (code_end == std::string_view::npos) return out;
    std::string_view code = raw.substr(sp1 + 1, code_end - sp1 - 1);
    std::from_chars(code.data(), code.data() + code.size(), out.status);
    const auto head_end = raw.find("\r\n\r\n");
    if (head_end != std::string_view::npos) out.body = std::string(raw.substr(head_end + 4));
    return out;
}

}  // namespace stratus::http

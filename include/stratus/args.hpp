// Command line flags of the form --name value. Shared by the load generator, the
// autoscaler and the cost calculator so that the three tools do not each invent their own
// slightly different parser.
#pragma once

#include <charconv>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace stratus::args {

class Args {
public:
    Args(int argc, char** argv) {
        for (int i = 1; i < argc; ++i) tokens_.emplace_back(argv[i]);
    }

    bool has(std::string_view name) const {
        for (const auto& t : tokens_) if (t == std::string("--") + std::string(name)) return true;
        return false;
    }

    std::optional<std::string> str(std::string_view name) const {
        const std::string key = "--" + std::string(name);
        for (std::size_t i = 0; i + 1 < tokens_.size(); ++i) {
            if (tokens_[i] == key) return tokens_[i + 1];
        }
        return std::nullopt;
    }

    std::string str_or(std::string_view name, std::string fallback) const {
        auto v = str(name);
        return v ? *v : std::move(fallback);
    }

    std::optional<double> num(std::string_view name) const {
        auto v = str(name);
        if (!v) return std::nullopt;
        try {
            std::size_t used = 0;
            const double d = std::stod(*v, &used);
            if (used != v->size()) return std::nullopt;
            return d;
        } catch (...) {
            return std::nullopt;
        }
    }

    double num_or(std::string_view name, double fallback) const {
        auto v = num(name);
        return v ? *v : fallback;
    }

    int int_or(std::string_view name, int fallback) const {
        auto v = num(name);
        return v ? static_cast<int>(*v) : fallback;
    }

private:
    std::vector<std::string> tokens_;
};

}  // namespace stratus::args

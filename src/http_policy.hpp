#pragma once
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <string>

// Reserve heap for TLS receive records and reconnect handshakes. Existing
// admitted streams always get to finish, even after crossing these thresholds.
class HttpAdmission {
    std::atomic<unsigned> active{0};

  public:
    static constexpr unsigned limit = 24;
    bool acquire(size_t freeBytes, size_t largestBlock) {
        if (freeBytes < 48000 || largestBlock < 20000)
            return false;
        unsigned value = active.load();
        while (value < limit) {
            if (active.compare_exchange_weak(value, value + 1))
                return true;
        }
        return false;
    }
    void release() { active.fetch_sub(1); }
    unsigned count() const { return active.load(); }
};

inline bool acceptsGzip(std::string header) {
    std::transform(header.begin(), header.end(), header.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    auto trim = [](std::string text) {
        auto first = text.find_first_not_of(" \t");
        if (first == std::string::npos)
            return std::string();
        return text.substr(first, text.find_last_not_of(" \t") - first + 1);
    };
    bool wildcard = false, explicitGzip = false, gzip = false;
    size_t at = 0;
    while (at < header.size()) {
        size_t end = header.find(',', at);
        if (end == std::string::npos)
            end = header.size();
        auto item = header.substr(at, end - at);
        auto semi = item.find(';');
        auto name = trim(item.substr(0, semi));
        double q = 1;
        while (semi != std::string::npos) {
            size_t next = item.find(';', semi + 1);
            auto param =
                trim(item.substr(semi + 1, next == std::string::npos ? next : next - semi - 1));
            auto equal = param.find('=');
            if (trim(param.substr(0, equal)) == "q") {
                auto value = equal == std::string::npos ? "" : trim(param.substr(equal + 1));
                char *tail = nullptr;
                q = strtod(value.c_str(), &tail);
                if (value.empty() || *tail || !(q >= 0 && q <= 1))
                    q = 0;
            }
            semi = next;
        }
        if (name == "gzip") {
            explicitGzip = true;
            gzip = q > 0;
        }
        if (name == "*")
            wildcard = q > 0;
        at = end + 1;
    }
    return explicitGzip ? gzip : wildcard;
}

#include "edge_dns.hpp"
#include <algorithm>

namespace edge_dns {
namespace {
uint16_t number(const std::vector<uint8_t> &b, size_t at) {
    return uint16_t(b[at]) << 8 | b[at + 1];
}
bool skipName(const std::vector<uint8_t> &b, size_t &at) {
    // Only skip encoded names; never follow untrusted compression pointers.
    for (unsigned labels = 0; labels < 128; ++labels) {
        if (at >= b.size())
            return false;
        uint8_t len = b[at++];
        if (!len)
            return true;
        if ((len & 0xc0) == 0xc0) {
            if (at >= b.size() || ((size_t(len & 63) << 8) | b[at]) >= b.size())
                return false;
            ++at;
            return true;
        }
        if (len > 63 || at + len > b.size())
            return false;
        at += len;
    }
    return false;
}
} // namespace
std::vector<uint8_t> query(const std::string &host, uint16_t id) {
    if (host.empty() || host.size() > 253)
        return {};
    std::vector<uint8_t> b = {uint8_t(id >> 8), uint8_t(id), 1, 0, 0, 1, 0, 0, 0, 0, 0, 0};
    size_t start = 0;
    while (start < host.size()) {
        auto end = host.find('.', start);
        if (end == std::string::npos)
            end = host.size();
        if (end <= start || end - start > 63)
            return {};
        b.push_back(end - start);
        b.insert(b.end(), host.begin() + start, host.begin() + end);
        start = end + 1;
    }
    b.insert(b.end(), {0, 0, 1, 0, 1});
    return b;
}
std::vector<Address> parse(const std::vector<uint8_t> &b, const std::vector<uint8_t> &request) {
    if (request.size() < 17 || b.size() < request.size() || b.size() > 4096 ||
        number(b, 0) != number(request, 0))
        return {};
    uint16_t flags = number(b, 2);
    if (!(flags & 0x8000) || (flags & 0x7800) || (flags & 0x0200) || (flags & 15) ||
        number(b, 4) != 1 || !std::equal(request.begin() + 12, request.end(), b.begin() + 12))
        return {};
    size_t at = request.size();
    std::vector<Address> result;
    unsigned answers = number(b, 6);
    if (answers > 128)
        return {};
    for (unsigned i = 0; i < answers; ++i) {
        if (!skipName(b, at) || at + 10 > b.size())
            return {};
        uint16_t type = number(b, at), klass = number(b, at + 2), length = number(b, at + 8);
        at += 10;
        if (at + length > b.size())
            return {};
        if (type == 1 && klass == 1 && length == 4) {
            Address ip{b[at], b[at + 1], b[at + 2], b[at + 3]};
            if (std::find(result.begin(), result.end(), ip) == result.end())
                result.push_back(ip);
        }
        at += length;
    }
    return result;
}
} // namespace edge_dns

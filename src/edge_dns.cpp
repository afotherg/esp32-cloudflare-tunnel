#include "edge_dns.hpp"
#include <algorithm>
#include <stdexcept>

namespace edge_dns {
namespace {
void check(bool ok) {
    if (!ok)
        throw std::runtime_error("invalid edge DNS response");
}
uint16_t number(const std::vector<uint8_t> &b, size_t at) {
    check(at + 2 <= b.size());
    return uint16_t(b[at]) << 8 | b[at + 1];
}
void skipName(const std::vector<uint8_t> &b, size_t &at) {
    // We only need the end of the name, not pointer expansion; never follow
    // untrusted compression pointers or allocate based on a DNS length field.
    for (unsigned labels = 0; labels < 128; ++labels) {
        check(at < b.size());
        uint8_t len = b[at++];
        if (!len)
            return;
        if ((len & 0xc0) == 0xc0) {
            check(at < b.size());
            check(((size_t(len & 63) << 8) | b[at]) < b.size());
            ++at;
            return;
        }
        check(len <= 63 && at + len <= b.size());
        at += len;
    }
    check(false);
}
} // namespace
std::vector<uint8_t> query(const std::string &host, uint16_t id) {
    check(!host.empty() && host.size() <= 253);
    std::vector<uint8_t> b = {uint8_t(id >> 8), uint8_t(id), 1, 0, 0, 1, 0, 0, 0, 0, 0, 0};
    size_t start = 0;
    while (start < host.size()) {
        auto end = host.find('.', start);
        if (end == std::string::npos)
            end = host.size();
        check(end > start && end - start <= 63);
        b.push_back(end - start);
        b.insert(b.end(), host.begin() + start, host.begin() + end);
        start = end + 1;
    }
    b.insert(b.end(), {0, 0, 1, 0, 1});
    return b;
}
std::vector<Address> parse(const std::vector<uint8_t> &b, const std::vector<uint8_t> &request) {
    check(request.size() >= 17 && b.size() >= request.size() && b.size() <= 4096);
    check(number(b, 0) == number(request, 0));
    uint16_t flags = number(b, 2);
    check((flags & 0x8000) && !(flags & 0x7800) && !(flags & 0x0200) && !(flags & 15));
    check(number(b, 4) == 1);
    check(std::equal(request.begin() + 12, request.end(), b.begin() + 12));
    size_t at = request.size();
    std::vector<Address> result;
    unsigned answers = number(b, 6);
    check(answers <= 128);
    for (unsigned i = 0; i < answers; ++i) {
        skipName(b, at);
        uint16_t type = number(b, at), klass = number(b, at + 2), length = number(b, at + 8);
        at += 10;
        check(at + length <= b.size());
        if (type == 1 && klass == 1 && length == 4) {
            Address ip{b[at], b[at + 1], b[at + 2], b[at + 3]};
            if (std::find(result.begin(), result.end(), ip) == result.end())
                result.push_back(ip);
        }
        at += length;
    }
    check(!result.empty());
    return result;
}
} // namespace edge_dns

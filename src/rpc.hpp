#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace rpc {
using Bytes = std::vector<uint8_t>;
struct Credentials {
    std::string account;
    Bytes secret, tunnel, client, ip;
};
Bytes bootstrap();
Bytes registration(const Credentials &credentials);
Bytes finish(uint32_t question);
// 0 means incomplete. Throws on malformed or oversized input.
size_t frameSize(const Bytes &bytes);
struct Reply {
    enum Kind { Other, Registered, Rejected } kind = Other;
    std::string detail;
};
Reply parse(const Bytes &frame);
} // namespace rpc

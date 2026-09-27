#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace edge_dns {
using Address = std::array<uint8_t, 4>;
// Invalid input, malformed replies, DNS errors, and no A records return an empty vector.
std::vector<uint8_t> query(const std::string &host, uint16_t id);
std::vector<Address> parse(const std::vector<uint8_t> &response,
                           const std::vector<uint8_t> &request);
} // namespace edge_dns

#include "connection_state.hpp"
#include "edge_dns.hpp"
#include <cassert>
#include <stdexcept>
#include <thread>
#include <vector>
int main() {
    ConnectionState state;
    std::vector<std::thread> workers;
    for (unsigned i = 0; i < 4; ++i)
        workers.emplace_back([&, i] {
            for (unsigned n = 0; n < 10000; ++n) {
                state.set(i, true);
                state.set(i, false);
            }
            state.set(i, true);
        });
    for (auto &worker : workers)
        worker.join();
    assert(ConnectionState::count(state.snapshot()) == 4);
    state.set(2, false);
    assert(state.snapshot() == 11);
    state.set(2, false);
    assert(state.snapshot() == 11);
    state.set(2, true);
    assert(state.snapshot() == 15);
    state.clear();
    assert(state.snapshot() == 0);
    state.set(4, true);
    assert(state.snapshot() == 0);
    auto query = edge_dns::query("region1.v2.argotunnel.com", 1234);
    auto response = query;
    response[2] = 0x81;
    response[3] = 0x80;
    response[7] = 3;
    for (unsigned i : {1, 2, 1}) {
        const uint8_t record[] = {0xc0, 0x0c, 0, 1, 0,   1,  0,   0,
                                  0,    60,   0, 4, 198, 41, 192, uint8_t(i)};
        response.insert(response.end(), std::begin(record), std::end(record));
    }
    auto addresses = edge_dns::parse(response, query);
    assert(addresses.size() == 2 && addresses[0][3] == 1 && addresses[1][3] == 2);
    auto rejected = [&](std::vector<uint8_t> data) {
        try {
            edge_dns::parse(data, query);
        } catch (const std::runtime_error &) {
            return;
        }
        assert(false);
    };
    for (size_t size = 0; size < response.size(); ++size)
        rejected(std::vector<uint8_t>(response.begin(), response.begin() + size));
    auto invalid = response;
    invalid[0] ^= 1;
    rejected(invalid);
    invalid = response;
    invalid[2] |= 2;
    rejected(invalid); // truncated UDP response
    invalid = response;
    invalid[3] |= 3;
    rejected(invalid); // NXDOMAIN
    invalid = response;
    invalid[12] ^= 1;
    rejected(invalid); // different question
    invalid = response;
    invalid[query.size()] = 0xff;
    rejected(invalid); // bad compression pointer
    invalid = response;
    invalid[query.size() + 11] = 255;
    rejected(invalid); // excessive RDATA
}

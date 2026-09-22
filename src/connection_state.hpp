#pragma once
#include <atomic>
#include <cstdint>

// Each worker owns one bit. Repeated registration and cleanup are idempotent,
// and losing one connection cannot clear another worker's healthy connection.
class ConnectionState {
    std::atomic<uint32_t> mask{0};

  public:
    static constexpr unsigned desired = 4;
    void set(unsigned index, bool value) {
        if (index >= desired)
            return;
        if (value)
            mask.fetch_or(1u << index);
        else
            mask.fetch_and(~(1u << index));
    }
    void clear() { mask.store(0); }
    uint32_t snapshot() const { return mask.load(); }
    static unsigned count(uint32_t bits) { return __builtin_popcount(bits & 15); }
};

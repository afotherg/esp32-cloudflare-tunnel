#include "http_policy.hpp"
#include <cassert>
#include <thread>
#include <vector>
int main() {
    for (const auto *header : {"gzip", "br, gzip", "GZip ; q=0.5", "*", "gzip;q=1, *;q=0"})
        assert(acceptsGzip(header));
    for (const auto *header : {"", "identity", "br", "gzip;q=0", "gzip;q=0,*;q=1", "gzip;q=bad",
                               "gzip;q=nan", "gzip;q=2", "gzip;q=-1"})
        assert(!acceptsGzip(header));
    HttpAdmission admission;
    assert(!admission.acquire(47999, 50000));
    assert(!admission.acquire(100000, 19999));
    std::vector<std::thread> workers;
    for (unsigned i = 0; i < 64; ++i)
        workers.emplace_back([&] { admission.acquire(100000, 50000); });
    for (auto &worker : workers)
        worker.join();
    assert(admission.count() == HttpAdmission::limit);
    assert(!admission.acquire(100000, 50000));
    for (unsigned i = 0; i < HttpAdmission::limit; ++i)
        admission.release();
    assert(admission.count() == 0);
    assert(admission.acquire(48000, 20000));
    admission.release();
}

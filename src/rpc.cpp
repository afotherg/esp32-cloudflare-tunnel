#include "rpc.hpp"
#include <cstring>
#include <stdexcept>

namespace rpc {
namespace {
constexpr size_t LIMIT = 16384;
void require(bool ok) {
    if (!ok)
        throw std::runtime_error("invalid Cap'n Proto frame");
}
uint64_t le(const uint8_t *p, size_t n) {
    uint64_t v = 0;
    for (size_t i = 0; i < n; ++i)
        v |= uint64_t(p[i]) << (i * 8);
    return v;
}
struct Writer {
    Bytes b = Bytes(8, 0);
    void put(size_t offset, uint64_t v, size_t count = 8) {
        for (size_t i = 0; i < count; ++i)
            b.at(offset + i) = v >> (8 * i);
    }
    size_t alloc(size_t words) {
        size_t n = b.size() / 8;
        b.resize(b.size() + words * 8);
        return n;
    }
    size_t structure(size_t ptr, size_t data, size_t pointers) {
        size_t n = alloc(data + pointers);
        put(ptr * 8,
            (uint64_t(n - ptr - 1) << 2) | (uint64_t(data) << 32) | (uint64_t(pointers) << 48));
        return n;
    }
    void bytes(size_t ptr, const Bytes &v) {
        size_t n = alloc((v.size() + 7) / 8);
        put(ptr * 8,
            1 | (uint64_t(n - ptr - 1) << 2) | (uint64_t(2) << 32) | (uint64_t(v.size()) << 35));
        if (!v.empty())
            std::memcpy(b.data() + n * 8, v.data(), v.size());
    }
    void text(size_t ptr, const std::string &v) {
        Bytes x(v.begin(), v.end());
        x.push_back(0);
        bytes(ptr, x);
    }
    void texts(size_t ptr, const std::vector<std::string> &items) {
        size_t n = alloc(items.size());
        put(ptr * 8, 1 | (uint64_t(n - ptr - 1) << 2) | (uint64_t(6) << 32) |
                         (uint64_t(items.size()) << 35));
        for (size_t i = 0; i < items.size(); ++i)
            text(n + i, items[i]);
    }
    size_t message(uint16_t type, size_t dw, size_t pw) {
        size_t m = structure(0, 1, 1);
        put(m * 8, type, 2);
        return structure(m + 1, dw, pw);
    }
    Bytes frame() {
        Bytes out(8 + b.size());
        uint32_t n = b.size() / 8;
        for (size_t i = 0; i < 4; ++i)
            out[4 + i] = n >> (8 * i);
        std::memcpy(out.data() + 8, b.data(), b.size());
        return out;
    }
};
struct Ref {
    size_t seg, word;
};
struct Struct {
    Ref r;
    size_t dw, pw;
};
struct Reader {
    const Bytes &b;
    std::vector<size_t> starts, sizes;
    explicit Reader(const Bytes &x) : b(x) {
        require(frameSize(b) == b.size());
        size_t count = le(b.data(), 4) + 1, pos = ((count + 2) & ~size_t(1)) * 4;
        for (size_t i = 0; i < count; ++i) {
            size_t words = le(b.data() + 4 + 4 * i, 4);
            starts.push_back(pos);
            sizes.push_back(words);
            pos += words * 8;
        }
    }
    uint64_t word(Ref r) const {
        require(r.seg < sizes.size() && r.word < sizes[r.seg]);
        return le(b.data() + starts[r.seg] + r.word * 8, 8);
    }
    std::pair<Ref, uint64_t> resolve(Ref p) const {
        uint64_t v = word(p);
        if ((v & 3) == 2) {
            Ref landing{size_t(v >> 32), size_t(uint32_t(v) >> 3)};
            if (v & 4) {
                uint64_t far = word(landing);
                require((far & 7) == 2);
                uint64_t tag = word({landing.seg, landing.word + 1});
                Ref target{size_t(far >> 32), size_t(uint32_t(far) >> 3)};
                require(target.seg < sizes.size() && target.word <= sizes[target.seg]);
                return {target, tag};
            }
            p = landing;
            v = word(p);
            require((v & 3) != 2);
        }
        int64_t delta = int32_t(uint32_t(v)) >> 2;
        int64_t dest = int64_t(p.word) + 1 + delta;
        require(dest >= 0 && size_t(dest) <= sizes[p.seg]);
        return {{p.seg, size_t(dest)}, v};
    }
    Struct structure(Ref p) const {
        auto v = resolve(p);
        require((v.second & 3) == 0 && v.second != 0);
        size_t dw = (v.second >> 32) & 65535, pw = v.second >> 48;
        require(v.first.word + dw + pw <= sizes[v.first.seg]);
        return {v.first, dw, pw};
    }
    Ref ptr(Struct s, size_t i) const {
        require(i < s.pw);
        return {s.r.seg, s.r.word + s.dw + i};
    }
    uint64_t data(Struct s, size_t off, size_t n) const {
        if (off + n > s.dw * 8)
            return 0;
        return le(b.data() + starts[s.r.seg] + s.r.word * 8 + off, n);
    }
    std::string text(Ref p) const {
        if (word(p) == 0)
            return {};
        auto v = resolve(p);
        require((v.second & 3) == 1 && ((v.second >> 32) & 7) == 2);
        size_t len = v.second >> 35;
        require(len > 0 && len <= 1024 && v.first.word * 8 + len <= sizes[v.first.seg] * 8);
        const char *str =
            reinterpret_cast<const char *>(b.data() + starts[v.first.seg] + v.first.word * 8);
        require(str[len - 1] == 0);
        return std::string(str, len - 1);
    }
};
} // namespace
Bytes bootstrap() {
    Writer w;
    w.message(8, 1, 1);
    return w.frame();
}
Bytes registration(const Credentials &c, uint8_t connectionIndex) {
    require(connectionIndex < 4);
    require(c.account.size() == 32 && !c.secret.empty() && c.secret.size() <= 128 &&
            c.tunnel.size() == 16 && c.client.size() == 16);
    Writer w;
    size_t call = w.message(2, 3, 3);
    w.put(call * 8, 1, 4);
    w.put((call + 1) * 8, 0xf71695ec7fe85497ULL);
    size_t target = w.structure(call + 3, 1, 1);
    w.put(target * 8 + 4, 1, 2);
    w.structure(target + 1, 1, 1); // target bootstrap question 0, no transform
    size_t payload = w.structure(call + 4, 0, 2);
    size_t params = w.structure(payload, 1, 3);
    w.put(params * 8, connectionIndex, 1);
    size_t auth = w.structure(params + 1, 0, 2);
    w.text(auth, c.account);
    w.bytes(auth + 1, c.secret);
    w.bytes(params + 2, c.tunnel);
    size_t opts = w.structure(params + 3, 1, 2);
    size_t client = w.structure(opts + 1, 0, 4);
    w.bytes(client, c.client);
    w.texts(client + 1, {"allow_remote_config", "serialized_headers"});
    w.text(client + 2, "esp32-native-0.4.0");
    w.text(client + 3, "espidf_esp32s3");
    w.bytes(opts + 2, c.ip);
    return w.frame();
}
Bytes finish(uint32_t q) {
    Writer w;
    size_t s = w.message(4, 1, 0);
    w.put(s * 8, q, 4);
    return w.frame();
}
size_t frameSize(const Bytes &b) {
    if (b.size() < 4)
        return 0;
    size_t count = le(b.data(), 4) + 1;
    require(count >= 1 && count <= 16);
    size_t header = ((count + 2) & ~size_t(1)) * 4;
    if (b.size() < header)
        return 0;
    size_t total = header;
    for (size_t i = 0; i < count; ++i) {
        size_t words = le(b.data() + 4 + 4 * i, 4);
        require(words <= LIMIT / 8);
        total += words * 8;
        require(total <= LIMIT);
    }
    return b.size() < total ? 0 : total;
}
Reply parse(const Bytes &frame) {
    Reader r(frame);
    auto m = r.structure({0, 0});
    auto type = r.data(m, 0, 2);
    if (type == 1)
        return {Reply::Rejected, r.text(r.ptr(r.structure(r.ptr(m, 0)), 0))};
    if (type != 3)
        return {};
    auto ret = r.structure(r.ptr(m, 0));
    uint32_t answer = r.data(ret, 0, 4);
    if (answer != 1)
        return {};
    if (r.data(ret, 6, 2) == 1)
        return {Reply::Rejected, r.text(r.ptr(r.structure(r.ptr(ret, 0)), 0))};
    require(r.data(ret, 6, 2) == 0);
    auto payload = r.structure(r.ptr(ret, 0));
    auto result = r.structure(r.ptr(payload, 0));
    auto response = r.structure(r.ptr(result, 0));
    auto details = r.structure(r.ptr(response, 0));
    if (r.data(response, 0, 2) == 0)
        return {Reply::Rejected, r.text(r.ptr(details, 0))};
    require(r.data(response, 0, 2) == 1);
    return {Reply::Registered, r.text(r.ptr(details, 1))};
}
} // namespace rpc

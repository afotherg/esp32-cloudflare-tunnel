#include "rpc.hpp"
#include <fstream>
#include <iostream>
#include <iterator>
int main(int argc, char **argv) {
    try {
        std::string mode = argc > 1 ? argv[1] : "";
        if (mode == "registration") {
            rpc::Credentials c;
            c.account = std::string(32, 'a');
            c.secret = rpc::Bytes(32, 0x53);
            c.tunnel = rpc::Bytes(16, 0x54);
            c.client = rpc::Bytes(16, 0x43);
            c.ip = {192, 168, 1, 2};
            auto b = rpc::registration(c, argc > 2 ? std::stoi(argv[2]) : 0);
            std::cout.write(reinterpret_cast<char *>(b.data()), b.size());
        } else if (mode == "bootstrap") {
            auto b = rpc::bootstrap();
            std::cout.write(reinterpret_cast<char *>(b.data()), b.size());
        } else {
            rpc::Bytes b(std::istreambuf_iterator<char>(std::cin), {});
            auto size = rpc::frameSize(b);
            if (!size) {
                std::cout << "incomplete";
                return 0;
            }
            auto r = rpc::parse(b);
            std::cout << int(r.kind) << ":" << r.detail;
        }
    } catch (const std::exception &e) {
        std::cerr << e.what();
        return 1;
    }
}

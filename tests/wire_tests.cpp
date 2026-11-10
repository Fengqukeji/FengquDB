#include "fengqudb_wire.hpp"
#include <iostream>
#include <fstream>
#include <iterator>
#include <random>
#include <stdexcept>
static int assertions = 0;
#define REQUIRE(x) do { ++assertions; if (!(x)) throw std::runtime_error(#x); } while (0)
static std::string fixture() {
    using namespace fengqudb::wire;
    Writer w;
    w.uint_field(1, UINT64_MAX);
    w.sint_field(2, INT64_MIN);
    w.fixed32_field(3, 0x12345678);
    w.fixed64_field(4, 0x123456789abcdef0ULL);
    w.bytes_field(5, std::string("A\0\xff", 3));
    w.float_field(6, 1.25f);
    w.double_field(7, -2.5);
    Writer packed;
    for (auto n : {0ULL, 127ULL, 128ULL, 4294967295ULL})
        packed.varint(n);
    w.bytes_field(8, packed.data());
    Writer nested;
    nested.uint_field(1, 150);
    w.bytes_field(9, nested.data());
    return w.take();
}
static void check_fixture(const std::string &bytes) {
    using namespace fengqudb::wire;
    Reader r(bytes);
    Field f;
    unsigned mask = 0;
    while (r.next(f)) {
        uint64_t value = 0;
        std::string_view data;
        switch (f.number) {
        case 1:
            REQUIRE(f.type == Type::Varint && r.varint(value) && value == UINT64_MAX);
            mask |= 1;
            break;
        case 2:
            REQUIRE(f.type == Type::Varint && r.varint(value) && unzigzag(value) == INT64_MIN);
            mask |= 2;
            break;
        case 3:
            REQUIRE(f.type == Type::Fixed32 && r.fixed(value, 4) && value == 0x12345678);
            mask |= 4;
            break;
        case 4:
            REQUIRE(f.type == Type::Fixed64 && r.fixed(value, 8) && value == 0x123456789abcdef0ULL);
            mask |= 8;
            break;
        case 5:
            REQUIRE(f.type == Type::Bytes && r.bytes(data) && data == std::string("A\0\xff", 3));
            mask |= 16;
            break;
        case 6:
            REQUIRE(f.type == Type::Fixed32 && r.fixed(value, 4) && value == 0x3fa00000);
            mask |= 32;
            break;
        case 7:
            REQUIRE(f.type == Type::Fixed64 && r.fixed(value, 8) && value == 0xc004000000000000ULL);
            mask |= 64;
            break;
        case 8: {
            REQUIRE(f.type == Type::Bytes && r.bytes(data));
            Reader p(data);
            for (auto expected : {0ULL, 127ULL, 128ULL, 4294967295ULL})
                REQUIRE(p.varint(value) && value == expected);
            REQUIRE(p.done());
            mask |= 128;
            break;
        }
        case 9: {
            REQUIRE(f.type == Type::Bytes && r.bytes(data));
            Reader p(data);
            Field n;
            REQUIRE(p.next(n) && n.number == 1 && p.varint(value) && value == 150 && p.done());
            mask |= 256;
            break;
        }
        default:
            REQUIRE(r.skip(f));
        }
    }
    REQUIRE(r.done() && mask == 511);
}
static void wire_tests() {
    using namespace fengqudb::wire;
    check_fixture(fixture());
    std::mt19937_64 rng(42);
    for (int n = 0; n < 10000; ++n) {
        uint64_t v = rng(), out = 0;
        Writer w;
        w.varint(v);
        Reader r(w.data());
        REQUIRE(r.varint(out) && out == v && r.done());
        REQUIRE(unzigzag(zigzag(static_cast<int64_t>(v))) == static_cast<int64_t>(v));
        for (size_t cut = 0; cut < w.data().size(); ++cut) {
            Reader partial(std::string_view(w.data()).substr(0, cut));
            REQUIRE(!partial.varint(out) && !partial.good());
        }
    }
    uint64_t value;
    Field f;
    for (auto bad : {std::string(11, '\x80'), std::string(9, '\xff') + "\x02"}) {
        Reader r(bad);
        REQUIRE(!r.varint(value) && !r.good());
    }
    for (auto bad :
         {std::string("\0", 1), std::string("\x0e", 1), std::string("\xff\xff\xff\xff\x10", 5)}) {
        Reader r(bad);
        REQUIRE(!r.next(f) && !r.good());
    }
    {
        std::string b = "\x08\x01";
        Reader r(b, 1);
        REQUIRE(!r.next(f) && !r.good());
    }
    {
        Writer w;
        w.tag(100, Type::StartGroup);
        w.uint_field(1, 9);
        w.tag(100, Type::EndGroup);
        Reader r(w.data());
        REQUIRE(r.next(f) && r.skip(f) && r.done());
    }
    {
        Writer w;
        w.tag(100, Type::StartGroup);
        w.tag(101, Type::EndGroup);
        Reader r(w.data());
        REQUIRE(r.next(f) && !r.skip(f));
    }
    {
        Writer w;
        for (int i = 0; i < 65; ++i)
            w.tag(1, Type::StartGroup);
        Reader r(w.data());
        REQUIRE(r.next(f) && !r.skip(f));
    }
    for (int n = 0; n < 20000; ++n) {
        std::string bytes(static_cast<size_t>(rng() % 64), '\0');
        for (auto &b : bytes)
            b = static_cast<char>(rng());
        Reader r(bytes);
        while (r.next(f)) {
            if (!r.skip(f))
                break;
        }
        REQUIRE(r.done() || !r.good());
    }
}

int main(int argc, char **argv) {
    try {
        if (argc == 3 && std::string(argv[1]) == "--wire-check") {
            std::ifstream in(argv[2], std::ios::binary);
            REQUIRE(bool(in));
            check_fixture(std::string(std::istreambuf_iterator<char>(in), {}));
        } else if (argc == 3 && std::string(argv[1]) == "--wire-emit") {
            std::ofstream out(argv[2], std::ios::binary);
            auto bytes = fixture();
            out.write(bytes.data(), bytes.size());
            REQUIRE(bool(out));
        } else {
            REQUIRE(argc == 1);
            wire_tests();
        }
        std::cout << "PASS wire: " << assertions << " checks\n";
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}

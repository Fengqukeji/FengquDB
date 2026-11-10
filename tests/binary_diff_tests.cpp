#include "binary_diff.hpp"
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
namespace d = fengqudb::detail;
static void check(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
static void verify(std::string_view old, std::string_view next) {
    const auto plan = d::binary_diff(old, next);
    if (!plan.accepted)
        return;
    check(old.size() == next.size() && plan.count <= 8, "invalid accepted plan");
    std::string rebuilt(old);
    size_t previous = 0, bytes = 4;
    for (uint32_t i = 0; i < plan.count; ++i) {
        const auto &range = plan.ranges[i];
        check(range.size && range.offset >= previous && range.offset <= next.size() &&
                  range.size <= next.size() - range.offset,
              "range bounds");
        rebuilt.replace(range.offset, range.size, next.data() + range.offset, range.size);
        previous = range.offset + range.size;
        bytes += 8 + range.size;
    }
    check(bytes == plan.bytes && bytes <= std::min<size_t>(next.size() / 4, 1024), "plan budget");
    check(rebuilt == next, "delta omitted a changed byte");
}
int main() try {
    check(d::binary_equal({}, {}), "empty equality");
    check(!d::binary_equal("a", "ab") && !d::binary_equal("a", "b"), "different equality");
    std::string old(4173, 'a'), next = old;
    next[10] = 'b';
    next.back() = 'c';
    auto plan = d::binary_diff(old, next);
    check(plan.accepted && plan.count == 2 && plan.ranges[0].offset == 10 &&
              plan.ranges[0].size == 1 && plan.ranges[1].offset == 4172 && plan.ranges[1].size == 1,
          "edge refinement");
    verify(old, next);
    check(d::binary_diff(old, old).accepted && !d::binary_diff(old, old).count, "unchanged plan");
    check(!d::binary_diff(std::string(65536, 'a'), std::string(65536, 'b')).accepted,
          "dense fallback");
    check(!d::binary_diff(std::string(65537, 'a'), std::string(65537, 'a')).accepted,
          "large fallback");
    check(!d::binary_diff("a", "a").accepted && !d::binary_diff(old, old + "x").accepted,
          "unsupported shape");
    std::mt19937 random(91);
    for (unsigned n = 0; n < 2000; ++n) {
        size_t size = 1024 + random() % 64513;
        old.assign(size, 'a');
        for (auto &v : old)
            v = char(random());
        next = old;
        if (n % 5 == 0)
            for (auto &v : next)
                v ^= 1;
        else if (n % 5 == 1)
            for (unsigned j = 0; j < 1 + random() % 30; ++j)
                next[random() % size] ^= char(1 + random() % 127);
        else if (n % 5 == 2)
            for (size_t j = 0; j < size / 3; ++j)
                next[j] ^= 1;
        else if (n % 5 == 3) {
            next[0] ^= 1;
            next.back() ^= 1;
        }
        // Views start at unaligned addresses and end at their exact byte limit.
        old.insert(old.begin(), '!');
        next.insert(next.begin(), '!');
        verify(std::string_view(old).substr(1), std::string_view(next).substr(1));
    }
    std::cout << "PASS bounded binary differences, dense fallback, exact reconstruction and "
                 "unaligned views\n";
} catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
}

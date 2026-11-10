#ifndef FENGQUDB_VALUE_BYTES_HPP
#define FENGQUDB_VALUE_BYTES_HPP
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <atomic>
#ifdef FQDB_TESTING
#include <atomic>
#endif

namespace fengqudb {
namespace detail {
using Bytes = std::string;
struct ValueBytes : Bytes {
    using Bytes::Bytes;
    explicit ValueBytes(Bytes bytes) : Bytes(std::move(bytes)) {}
    mutable std::atomic<uint64_t> cached_crc{0};
};
using Value = std::shared_ptr<const ValueBytes>;
inline int compare(std::string_view a, std::string_view b) noexcept {
    const size_t length = std::min(a.size(), b.size());
    const int cmp = length ? std::memcmp(a.data(), b.data(), length) : 0;
    return cmp ? cmp : (a.size() > b.size()) - (a.size() < b.size());
}
struct ByteLess {
    using is_transparent = void;
    bool operator()(std::string_view a, std::string_view b) const noexcept {
        return compare(a, b) < 0;
    }
};
// Small keys stay beside child pointers, avoiding a separately allocated string
// on each point lookup. Larger keys retain shared immutable storage on path copy.
struct Key {
    uint32_t size;
    std::array<char, 24> inline_bytes{};
    uint8_t delta_depth = 0;
    uint8_t height = 1; // Packs into otherwise unused key-header padding.
    Value external;
    explicit Key(const Bytes &bytes) : size(static_cast<uint32_t>(bytes.size())) {
        if (size <= inline_bytes.size()) {
            if (size)
                std::memcpy(inline_bytes.data(), bytes.data(), size);
        } else
            external = std::make_shared<const ValueBytes>(bytes);
    }
    std::string_view view() const noexcept {
        return {external ? external->data() : inline_bytes.data(), size};
    }
};
} // namespace detail
} // namespace fengqudb
#endif

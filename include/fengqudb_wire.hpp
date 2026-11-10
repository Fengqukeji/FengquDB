#ifndef FENGQUDB_WIRE_HPP
#define FENGQUDB_WIRE_HPP
// Original, bounded Protobuf wire primitives. No Google runtime/source dependency.
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace fengqudb {
namespace wire {
enum class Type : uint8_t {
    Varint = 0,
    Fixed64 = 1,
    Bytes = 2,
    StartGroup = 3,
    EndGroup = 4,
    Fixed32 = 5
};
struct Field {
    uint32_t number = 0;
    Type type = Type::Varint;
};
inline constexpr size_t varint_size(uint64_t value) noexcept {
    size_t result = 1;
    while (value >= 128) {
        value >>= 7;
        ++result;
    }
    return result;
}
inline size_t bytes_size(uint32_t field, size_t length) noexcept {
    return varint_size((static_cast<uint64_t>(field) << 3) | 2) + varint_size(length) + length;
}
inline uint64_t zigzag(int64_t value) noexcept {
    return (static_cast<uint64_t>(value) << 1) ^ (value < 0 ? UINT64_MAX : 0);
}
inline int64_t unzigzag(uint64_t value) noexcept {
    return static_cast<int64_t>(value >> 1) ^ -static_cast<int64_t>(value & 1);
}
class Writer {
    std::string bytes_;

  public:
    Writer() = default;
    explicit Writer(std::string storage) : bytes_(std::move(storage)) {
        bytes_.clear();
    }
    void reserve(size_t size) {
        bytes_.reserve(size);
    }
    void clear() noexcept {
        bytes_.clear();
    }
    void varint(uint64_t value) {
        while (value >= 128) {
            bytes_.push_back(static_cast<char>((value & 127) | 128));
            value >>= 7;
        }
        bytes_.push_back(static_cast<char>(value));
    }
    void tag(uint32_t field, Type type) {
        if (!field || field > 0x1fffffff || static_cast<uint8_t>(type) > 5)
            throw std::invalid_argument("invalid Protobuf tag");
        varint((static_cast<uint64_t>(field) << 3) | static_cast<uint8_t>(type));
    }
    void uint_field(uint32_t field, uint64_t value) {
        tag(field, Type::Varint);
        varint(value);
    }
    void sint_field(uint32_t field, int64_t value) {
        uint_field(field, zigzag(value));
    }
    void fixed(uint64_t value, unsigned count) {
        if (count != 4 && count != 8)
            throw std::invalid_argument("fixed width");
        for (unsigned i = 0; i < count; ++i)
            bytes_.push_back(static_cast<char>((value >> (8 * i)) & 255));
    }
    void fixed32_field(uint32_t field, uint32_t value) {
        tag(field, Type::Fixed32);
        fixed(value, 4);
    }
    void fixed64_field(uint32_t field, uint64_t value) {
        tag(field, Type::Fixed64);
        fixed(value, 8);
    }
    void float_field(uint32_t field, float value) {
        static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559,
                      "IEEE float required");
        uint32_t bits;
        std::memcpy(&bits, &value, 4);
        fixed32_field(field, bits);
    }
    void double_field(uint32_t field, double value) {
        static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559,
                      "IEEE double required");
        uint64_t bits;
        std::memcpy(&bits, &value, 8);
        fixed64_field(field, bits);
    }
    void bytes_field(uint32_t field, std::string_view value) {
        tag(field, Type::Bytes);
        varint(value.size());
        if (!value.empty())
            bytes_.append(value.data(), value.size());
    }
    void message_header(uint32_t field, size_t length) {
        tag(field, Type::Bytes);
        varint(length);
    }
    const std::string &data() const noexcept {
        return bytes_;
    }
    std::string take() {
        return std::move(bytes_);
    }
};

class Reader {
    std::string_view bytes_;
    size_t position_ = 0;
    bool valid_ = true;
    bool fail() noexcept {
        valid_ = false;
        return false;
    }
    bool skip_impl(Field field, unsigned depth) noexcept {
        uint64_t number;
        std::string_view value;
        switch (field.type) {
        case Type::Varint:
            return varint(number);
        case Type::Fixed64:
            return fixed(number, 8);
        case Type::Bytes:
            return bytes(value);
        case Type::Fixed32:
            return fixed(number, 4);
        case Type::StartGroup: {
            if (depth >= 64)
                return fail();
            Field nested;
            while (next(nested)) {
                if (nested.type == Type::EndGroup)
                    return nested.number == field.number ? true : fail();
                if (!skip_impl(nested, depth + 1))
                    return false;
            }
            return fail();
        }
        default:
            return fail();
        }
    }

  public:
    explicit Reader(std::string_view bytes, size_t limit = 67108864) noexcept
        : bytes_(bytes), valid_(bytes.size() <= limit) {}
    bool good() const noexcept {
        return valid_;
    }
    bool done() const noexcept {
        return valid_ && position_ == bytes_.size();
    }
    size_t position() const noexcept {
        return position_;
    }
    bool varint(uint64_t &value) noexcept {
        if (!valid_)
            return false;
        value = 0;
        for (unsigned i = 0; i < 10; ++i) {
            if (position_ == bytes_.size())
                return fail();
            auto byte = static_cast<uint8_t>(bytes_[position_++]);
            if (i == 9 && byte > 1)
                return fail();
            value |= static_cast<uint64_t>(byte & 127) << (7 * i);
            if (!(byte & 128))
                return true;
        }
        return fail();
    }
    bool next(Field &field) noexcept {
        if (!valid_ || position_ == bytes_.size())
            return false;
        uint64_t tag;
        if (!varint(tag) || tag > UINT32_MAX || !(tag >> 3) || (tag & 7) > 5)
            return fail();
        field = {static_cast<uint32_t>(tag >> 3), static_cast<Type>(tag & 7)};
        return true;
    }
    bool fixed(uint64_t &value, unsigned count) noexcept {
        if (!valid_ || (count != 4 && count != 8) || count > bytes_.size() - position_)
            return fail();
        value = 0;
        for (unsigned i = 0; i < count; ++i)
            value |= static_cast<uint64_t>(static_cast<uint8_t>(bytes_[position_++])) << (8 * i);
        return true;
    }
    bool bytes(std::string_view &value) noexcept {
        uint64_t length;
        if (!varint(length) || length > bytes_.size() - position_)
            return fail();
        value = bytes_.substr(position_, static_cast<size_t>(length));
        position_ += static_cast<size_t>(length);
        return true;
    }
    bool skip(Field field) noexcept {
        return valid_ && skip_impl(field, 0);
    }
};
} // namespace wire
} // namespace fengqudb
#endif

#pragma once
#include "budget.hpp"
#include "fengqudb_wire.hpp"

namespace fengqudb::paged {
inline void validate_projection(const uint32_t *fields, uint32_t count) {
    require(count <= 256 && (!count || fields), FQDB_INVALID_ARGUMENT);
    for (uint32_t i = 0; i < count; ++i)
        require(fields[i] && fields[i] <= 0x1fffffff, FQDB_INVALID_ARGUMENT);
}
inline std::vector<uint32_t> projection_fields(const uint32_t *fields, uint32_t count) {
    validate_projection(fields, count);
    std::vector<uint32_t> result;
    result.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        result.push_back(fields[i]);
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}
inline Bytes project_fields(std::string_view bytes, const std::vector<uint32_t> &fields,
                            Charge &charge, uint64_t &retained) {
    // Common projections parse once. Coalesce adjacent selected fields and
    // retain only bounded metadata; pathological fragmentation uses two passes.
    std::array<std::string_view, 64> spans;
    size_t count = 0, length = 0;
    bool overflow = false;
    auto visit = [&](auto &&consume) {
        wire::Reader reader(bytes);
        wire::Field field;
        while (!reader.done()) {
            const auto start = reader.position();
            require(reader.next(field) && reader.skip(field), FQDB_CONSTRAINT);
            if (std::binary_search(fields.begin(), fields.end(), field.number))
                consume(bytes.substr(start, reader.position() - start));
        }
        require(reader.done(), FQDB_CONSTRAINT);
    };
    visit([&](std::string_view part) {
        length += part.size();
        if (overflow)
            return;
        if (count && spans[count - 1].data() + spans[count - 1].size() == part.data())
            spans[count - 1] = {spans[count - 1].data(), spans[count - 1].size() + part.size()};
        else if (count < spans.size())
            spans[count++] = part;
        else
            overflow = true;
    });
    require(retained <= FQDB_MAX_TRANSACTION_BYTES &&
                length <= FQDB_MAX_TRANSACTION_BYTES - retained &&
                retained + length <= FQDB_MAX_TRANSACTION_BYTES - 64,
            FQDB_LIMIT);
    retained += length + 64;
    charge.resize(retained);
    Bytes result;
    result.reserve(length);
    if (overflow)
        visit([&](std::string_view part) { result.append(part); });
    else
        for (size_t i = 0; i < count; ++i)
            result.append(spans[i]);
    return result;
}
} // namespace fengqudb::paged

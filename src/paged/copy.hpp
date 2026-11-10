#pragma once
#include "recovery.hpp"

namespace fengqudb::paged {
// Copy one final value into a compact baseline. Plain values use bounded I/O
// blocks; patch recipes are collapsed once using the subtraction value reader.
// CRC combination lets the record header be written before streaming bytes.
template <class Check>
inline Ref copy_value(Generation &source, const Ref &input, uint64_t limit, Generation &target,
                      std::string_view key, uint64_t time, Check &&check) {
    valid_ref(input, limit);
    require(!target.write_failed, FQDB_COMMIT_UNKNOWN);
    wire::Writer prefix;
    prefix.fixed64_field(2, time);
    wire::Writer op;
    op.uint_field(1, static_cast<uint32_t>(OpKind::put));
    op.bytes_field(2, key);
    op.fixed64_field(4, 0);
    op.fixed64_field(7, input.expires);
    op.fixed32_field(8, input.crc);
    op.fixed64_field(9, input.size);
    op.message_header(3, static_cast<size_t>(input.size));
    prefix.message_header(1, op.data().size() + static_cast<size_t>(input.size));
    auto header = prefix.take();
    header.append(op.data());
    const auto size = header.size() + input.size;
    require(size <= FQDB_MAX_TRANSACTION_BYTES && target.tail <= INT64_MAX &&
                size + 56 <= INT64_MAX - target.tail,
            FQDB_LIMIT);
    auto crc =
        storage::crc32_combine(storage::crc32(header.data(), header.size()), input.crc, input.size);
    storage::RecordFrame frame(wal_magic, input.version, static_cast<size_t>(size), crc);
    auto start = target.tail, offset = start + 32 + header.size();
    IOWritePermit permit(target.file->reservation_bytes(start,size + 56));
    Value materialized;
    if (input.depth)
        materialized = source.value(input, limit, false, false);
    check();
    try {
        // Gather only the small metadata; large unchanged byte ranges never
        // pass through a rebuilt transaction-sized concatenation buffer.
        Bytes metadata(reinterpret_cast<const char *>(frame.header.data()), frame.header.size());
        metadata.append(header);
        target.file->write(start, metadata.data(), metadata.size());
        uint32_t actual = 0;
        if (materialized) {
            actual = value_crc(materialized);
            target.file->write(offset, materialized->data(), materialized->size());
        } else {
            std::array<char, 262144> buffer;
            uint64_t position = 0;
            while (position < input.size) {
                check();
                auto count =
                    static_cast<size_t>(std::min<uint64_t>(buffer.size(), input.size - position));
                source.file->read(input.offset + position, buffer.data(), count);
                actual = storage::crc32(buffer.data(), count, actual);
                target.file->write(offset + position, buffer.data(), count);
                position += count;
            }
        }
        require(actual == input.crc);
        check();
        target.file->write(start + 32 + size, frame.trailer.data(), frame.trailer.size());
        target.tail += size + 56;
    } catch (...) {
        target.write_failed = true;
        throw;
    }
    auto output = input;
    output.offset = offset;
    output.depth = output.recipe_size = 0;
    return output;
}

// Reclaim only unselected, canonical generations of this exact database UUID.
// An exclusive lease and a verified header are required before deletion. Pinned
// generations in this or another process remain locked and are skipped.
inline uint64_t reclaim_generations(const std::string &directory, const Uuid &uuid,
                                    uint64_t selected,
                                    const std::vector<uint64_t> &keep = {}) noexcept {
    uint64_t removed = 0;
    try {
        const auto prefix = "gen-" + hex(uuid) + "-";
        std::error_code error;
        std::filesystem::directory_iterator cursor(std::filesystem::u8path(directory), error), end;
        size_t visited = 0;
        while (!error && cursor != end && visited < 4096) {
            auto entry = *cursor;
            cursor.increment(error);
            auto name = entry.path().filename().u8string();
            if (!generation_filename(name) ||
                (name.size() == 57 && name.compare(0, prefix.size(), prefix)))
                continue;
            ++visited;
            uint64_t number = 0;
            bool valid = true;
            for (size_t i = name.size() == 21 ? 1 : 37; i < name.size() - 4; ++i) {
                auto ch = name[i];
                if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) {
                    valid = false;
                    break;
                }
                number =
                    (number << 4) | static_cast<unsigned>(ch <= '9' ? ch - '0' : ch - 'a' + 10);
            }
            if (!valid || !number || number == selected ||
                std::find(keep.begin(), keep.end(), number) != keep.end() ||
                (name != generation_name(uuid, number) &&
                 name != legacy_generation_name(uuid, number)))
                continue;
            std::error_code type_error;
            if (entry.is_symlink(type_error) || type_error || !entry.is_regular_file(type_error) ||
                type_error)
                continue;
            try {
                Generation old(0);
                old.open(entry.path().u8string(), uuid, number, true);
                old.remove_on_close.store(true, std::memory_order_relaxed);
                ++removed;
            } catch (...) {
            } // Contention/corruption is preservation, not evidence of ownership.
        }
        if (removed)
            (void)storage::sync_parent(join(directory, "CURRENT").c_str());
    } catch (...) {
    }
    return removed;
}
} // namespace fengqudb::paged

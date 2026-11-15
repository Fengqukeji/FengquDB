#pragma once
#include "cache.hpp"
#include "delta_index.hpp"
#include "path.hpp"
#include <chrono>
#include <functional>
#include <optional>
#ifdef _WIN32
#include <bcrypt.h>
#elif defined(__linux__) && !defined(__ANDROID__)
#include <sys/random.h>
#endif

namespace fengqudb::paged {
constexpr uint64_t data_start = 12288;
constexpr char wal_magic[] = "FQTXN004", leaf_magic[] = "FQLEAF04", branch_magic[] = "FQNODE04",
               seal_magic[] = "FQCKPT04";
using Value = detail::Value;
using Data = detail::ValueBytes;
using Uuid = std::array<unsigned char, 16>;
#ifdef FQDB_TESTING
inline std::atomic<uint64_t> test_unix_ms{0};
inline std::atomic<uint64_t> test_segment_geometry{0};
#endif
inline uint64_t new_segment_geometry() {
#ifdef FQDB_TESTING
    if (auto geometry = test_segment_geometry.load(std::memory_order_relaxed))
        return geometry;
#endif
    return UINT64_C(64) << 20;
}
inline uint64_t unix_ms() {
#ifdef FQDB_TESTING
    if (auto now = test_unix_ms.load(std::memory_order_relaxed))
        return now;
#endif
    auto n = std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::system_clock::now().time_since_epoch())
                 .count();
    return n < 0 ? 0 : static_cast<uint64_t>(n);
}
inline Uuid new_uuid() {
    Uuid result{};
#ifdef _WIN32
    require(BCryptGenRandom(nullptr, result.data(), static_cast<ULONG>(result.size()),
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0, FQDB_IO_ERROR);
#else
#if !defined(__linux__) || defined(__ANDROID__)
    const int descriptor = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    require(descriptor >= 0, FQDB_IO_ERROR);
    struct Close {
        int descriptor;
        ~Close() { ::close(descriptor); }
    } close{descriptor};
#endif
    size_t completed = 0;
    while (completed < result.size()) {
        ssize_t count;
        do {
#if defined(__linux__) && !defined(__ANDROID__)
            count = ::getrandom(result.data() + completed, result.size() - completed, 0);
#else
            count = ::read(descriptor, result.data() + completed, result.size() - completed);
#endif
        } while (count < 0 && errno == EINTR);
        require(count > 0, FQDB_IO_ERROR);
        completed += static_cast<size_t>(count);
    }
#endif
    // Zero is reserved for absent identities. Entropy failures must never
    // silently publish that sentinel as a fresh database/backup identity.
    require(std::any_of(result.begin(), result.end(), [](unsigned char b) { return b != 0; }),
            FQDB_IO_ERROR);
    return result;
}
inline std::string hex(const Uuid &uuid) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(32);
    for (auto byte : uuid) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 15]);
    }
    return result;
}
inline std::string generation_name(const Uuid &, uint64_t generation) {
    char number[17];
    constexpr char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < 16; ++i)
        number[15 - i] = digits[(generation >> (4 * i)) & 15];
    number[16] = 0;
    return std::string("g") + number + ".fdb";
}
inline std::string legacy_generation_name(const Uuid &uuid, uint64_t generation) {
    return "gen-" + hex(uuid) + "-" + generation_name(uuid, generation).substr(1);
}
inline bool generation_filename(std::string_view name) {
    const bool short_name = name.size() == 21 && name[0] == 'g' && name.substr(17) == ".fdb";
    const bool legacy = name.size() == 57 && name.substr(0, 4) == "gen-" && name[36] == '-' &&
                        name.substr(53) == ".fdb";
    if (!short_name && !legacy)
        return false;
    for (size_t i = short_name ? 1 : 4; i < (short_name ? 17 : 53); ++i)
        if (i != 36 && !((name[i] >= '0' && name[i] <= '9') || (name[i] >= 'a' && name[i] <= 'f')))
            return false;
    return true;
}
inline std::string temporary_name(char kind, const Uuid &uuid) {
    return std::string(1, kind) + hex(uuid).substr(0, 16) + ".tmp";
}
inline std::string join(const std::string &directory, const std::string &name) {
    auto result = std::filesystem::u8path(directory) / std::filesystem::u8path(name);
    check_path(result);
    return result.u8string();
}
struct Current {
    Uuid uuid{};
    uint64_t generation = 0;
    bool short_name = true;
    Bytes filename() const {
        return short_name ? generation_name(uuid, generation)
                          : legacy_generation_name(uuid, generation);
    }
};
inline Current read_current(const std::string &directory) {
    SharedFile file;
    file.open(join(directory, "CURRENT"), false, true, true);
    auto length = file.size();
    require(length >= 32 && length <= 4096);
    std::array<unsigned char, 32> header{};
    file.read(0, header.data(), header.size());
    require(std::memcmp(header.data(), "FQCUR004", 8) == 0 &&
            storage::get32(header.data() + 8) == 4);
    require(storage::crc32(header.data(), 28) == storage::get32(header.data() + 28));
    auto size = storage::get32(header.data() + 12);
    require(size == length - 32);
    Bytes payload(size, '\0');
    file.read(32, payload.data(), payload.size());
    require(storage::crc32(payload.data(), payload.size()) == storage::get32(header.data() + 24));
    Current result;
    Bytes filename;
    unsigned seen = 0;
    wire::Reader reader(payload);
    wire::Field field;
    while (reader.next(field)) {
        if (field.number == 1 || field.number == 3) {
            std::string_view value;
            require(field.type == wire::Type::Bytes && reader.bytes(value));
            if (field.number == 1) {
                require(value.size() == result.uuid.size());
                std::memcpy(result.uuid.data(), value.data(), value.size());
                seen |= 1;
            } else {
                filename.assign(value);
                seen |= 4;
            }
        } else if (field.number == 2) {
            require(field.type == wire::Type::Varint && reader.varint(result.generation));
            seen |= 2;
        } else
            require(reader.skip(field));
    }
    if (filename == legacy_generation_name(result.uuid, result.generation))
        result.short_name = false;
    require(reader.done() && seen == 7 && result.generation && filename == result.filename() &&
            storage::get64(header.data() + 16) == result.generation);
    return result;
}
inline fqdb_status write_current(const std::string &directory, const Current &current) {
    auto target = join(directory, "CURRENT");
    auto temporary = join(directory, "CURRENT.tmp");
    bool created = false;
    try {
        wire::Writer payload;
        payload.bytes_field(
            1, {reinterpret_cast<const char *>(current.uuid.data()), current.uuid.size()});
        payload.uint_field(2, current.generation);
        payload.bytes_field(3, current.filename());
        std::array<unsigned char, 32> header{};
        std::memcpy(header.data(), "FQCUR004", 8);
        storage::put32(header.data() + 8, 4);
        storage::put32(header.data() + 12, static_cast<uint32_t>(payload.data().size()));
        storage::put64(header.data() + 16, current.generation);
        storage::put32(header.data() + 24,
                       storage::crc32(payload.data().data(), payload.data().size()));
        storage::put32(header.data() + 28, storage::crc32(header.data(), 28));
        IOWritePermit permit(header.size() + payload.data().size());
        storage::File file;
        auto status = file.open(temporary.c_str(), true);
        require(status == FQDB_OK, status);
        created = true;
        require(file.write(header.data(), header.size()) &&
                    file.write(payload.data().data(), payload.data().size()) && file.sync(),
                FQDB_IO_ERROR);
        file.close();
        auto result = publish_manifest(temporary, target);
        if (result != FQDB_OK) {
            remove_owned(temporary);
            return result;
        }
        created = false;
        // publish_manifest already persists the directory entry on POSIX and
        // uses write-through publication on Windows. Do not sync it twice.
        return FQDB_OK;
    } catch (...) {
        if (created)
            remove_owned(temporary);
        throw;
    }
}
struct Frame {
    enum class Kind { wal, leaf, branch, seal } kind;
    uint64_t offset = 0, version = 0, payload = 0, end = 0;
    uint32_t length = 0, crc = 0;
};
struct Checkpoint {
    uint64_t root = 0, version = 0, end = data_start, sequence = 0;
    uint64_t entries = 0, user_keys = 0, live_bytes = 0, time = 0, floor = 0;
    uint64_t features = 0, origin_hi = 0, origin_lo = 0;
    uint64_t parent = 0, parent_version = 0, parent_limit = 0, parent_legacy = 0,
             retained_floor = 0;
    uint64_t history_frame = 0, history_start = 0;
};
struct ReadEpoch {};
struct RetiredReadEpochs;
class Generation {
    void read_super(unsigned slot, std::array<unsigned char, 64> &bytes) {
        file->read(4096 + slot * 4096, bytes.data(), bytes.size());
    }

  public:
    std::shared_ptr<SharedFile> file = std::make_shared<SharedFile>();
    std::shared_ptr<ReadEpoch> reader_epoch = std::make_shared<ReadEpoch>();
    // Writer-protected publication. A cancelled reclaim must not forget older
    // readers before another session sweeps this same physical generation.
    std::shared_ptr<const RetiredReadEpochs> retired_readers;
    Uuid uuid{};
    uint64_t number = 0, tail = data_start;
    std::string path;
    Cache<Page> pages;
    Cache<Data> values;
    std::atomic<bool> remove_on_close{false};
    bool owns_path = false;
    bool write_failed = false; // A partial write requires reopen/recovery, never an append retry.
    explicit Generation(uint64_t cache_bytes)
        : pages(cache_bytes / 4), values(cache_bytes - cache_bytes / 4) {}
    ~Generation() {
        if (owns_path && remove_on_close.load(std::memory_order_relaxed))
            file->remove_files(path);
        file.reset();
    }
    void create(const std::string &name, Uuid id, uint64_t generation, uint64_t geometry = 0,
                bool accelerate = false) {
        IOWritePermit permit(geometry ? 192 : 64);
        path = name;
        uuid = id;
        number = generation;
        try {
            if (geometry)
                file->create_segmented(path, uuid, number, geometry);
            else
                file->open(path, true);
        } catch (...) {
            owns_path = file->exclusively_created();
            throw;
        }
        owns_path = true;
        file->lock();
        if (accelerate && !geometry && !storage::io_quota &&
            std::filesystem::u8path(path).extension() == ".fdb")
            file->initialize_arena(id, generation);
        std::array<unsigned char, 64> header{};
        std::memcpy(header.data(), "FQDATA04", 8);
        storage::put32(header.data() + 8, 4);
        storage::put32(header.data() + 12, 64);
        std::memcpy(header.data() + 16, uuid.data(), uuid.size());
        storage::put64(header.data() + 32, number);
        storage::put64(header.data() + 40, unix_ms());
        storage::put32(header.data() + 48, 1); // Understood record/page feature set.
        storage::put32(header.data() + 60, storage::crc32(header.data(), 60));
        file->write(0, header.data(), header.size());
        file->truncate(data_start);
    }
    void open(const std::string &name, Uuid id, uint64_t generation, bool read_only = false) {
        path = name;
        file->open(path, false, true, read_only);
        file->lock();
        std::array<unsigned char, 64> header{};
        file->read(0, header.data(), header.size());
        require(std::memcmp(header.data(), "FQDATA04", 8) == 0 &&
                    storage::get32(header.data() + 8) == 4,
                FQDB_UNSUPPORTED);
        require(storage::get32(header.data() + 12) == 64 &&
                storage::crc32(header.data(), 60) == storage::get32(header.data() + 60));
        require(storage::get32(header.data() + 48) == 1, FQDB_UNSUPPORTED);
        require(std::memcmp(header.data() + 16, id.data(), id.size()) == 0 &&
                storage::get64(header.data() + 32) == generation);
        uuid = id;
        number = generation;
        file->arena_identity(id, generation);
        tail = file->size();
        require(tail >= data_start);
        owns_path = true;
    }
    std::optional<Frame> frame(uint64_t offset, uint64_t limit, bool verify_payload = true) {
        require(offset >= data_start && offset <= limit);
        if (limit - offset < 32)
            return {};
        std::array<unsigned char, 32> header{};
        file->read(offset, header.data(), header.size());
        require(storage::get32(header.data() + 28) == 0 &&
                storage::crc32(header.data(), 24) == storage::get32(header.data() + 24));
        Frame result;
        result.offset = offset;
        result.payload = offset + 32;
        result.version = storage::get64(header.data() + 8);
        result.length = storage::get32(header.data() + 16);
        result.crc = storage::get32(header.data() + 20);
        if (!std::memcmp(header.data(), wal_magic, 8))
            result.kind = Frame::Kind::wal;
        else if (!std::memcmp(header.data(), leaf_magic, 8))
            result.kind = Frame::Kind::leaf;
        else if (!std::memcmp(header.data(), branch_magic, 8))
            result.kind = Frame::Kind::branch;
        else if (!std::memcmp(header.data(), seal_magic, 8))
            result.kind = Frame::Kind::seal;
        else
            throw Error(FQDB_CORRUPT);
        require(result.length &&
                result.length <= (result.kind == Frame::Kind::wal ? FQDB_MAX_TRANSACTION_BYTES
                                                                  : max_page_bytes));
        if (static_cast<uint64_t>(result.length) + 56 > limit - offset)
            return {};
        result.end = offset + result.length + 56;
        std::array<unsigned char, 24> trailer{};
        file->read(result.payload + result.length, trailer.data(), trailer.size());
        require(!std::memcmp(trailer.data(), storage::end_magic, 8) &&
                storage::get64(trailer.data() + 8) == result.version &&
                storage::get32(trailer.data() + 16) == result.crc &&
                storage::crc32(trailer.data(), 20) == storage::get32(trailer.data() + 20));
        if (verify_payload) {
            std::array<unsigned char, 65536> buffer{};
            uint64_t position = result.payload, remaining = result.length;
            uint32_t crc = 0;
            while (remaining) {
                auto size = static_cast<size_t>(std::min<uint64_t>(remaining, buffer.size()));
                file->read(position, buffer.data(), size);
                crc = storage::crc32(buffer.data(), size, crc);
                position += size;
                remaining -= size;
            }
            require(crc == result.crc);
        }
        return result;
    }
    uint64_t append(const char *magic, uint64_t version, std::string_view payload) {
        require(!write_failed, FQDB_COMMIT_UNKNOWN);
        require(!payload.empty() && payload.size() <= FQDB_MAX_TRANSACTION_BYTES &&
                    tail <= INT64_MAX && payload.size() + 56 <= INT64_MAX - tail,
                FQDB_LIMIT);
        storage::RecordFrame record(magic, version, payload);
        auto result = tail;
        IOWritePermit permit(file->reservation_bytes(tail,payload.size() + 56));
        try {
            if (payload.size() + 56 <= 4096) {
                std::array<char, 4096> buffer;
                std::memcpy(buffer.data(), record.header.data(), 32);
                std::memcpy(buffer.data() + 32, payload.data(), payload.size());
                std::memcpy(buffer.data() + 32 + payload.size(), record.trailer.data(), 24);
                file->write(tail, buffer.data(), payload.size() + 56);
            } else {
                file->write(tail, record.header.data(), 32);
                file->write(tail + 32, payload.data(), payload.size());
                file->write(tail + 32 + payload.size(), record.trailer.data(), 24);
            }
            tail += payload.size() + 56;
            return result;
        } catch (...) {
            write_failed = true;
            throw;
        }
    }
    std::shared_ptr<const Page> page(uint64_t offset, uint64_t limit, bool cached = true);
    Value value(const Ref &reference, uint64_t limit, bool retain = true, bool cached = true);
    void configure_cache(uint64_t bytes) {
        pages.configure(bytes / 4);
        values.configure(bytes - bytes / 4);
    }
    uint64_t write_page(const Page &page, uint64_t version) {
        require(page.binary.size() <= max_page_bytes && !page.slots.empty(), FQDB_LIMIT);
        return append(page.leaf ? leaf_magic : branch_magic, version, page.binary);
    }
    Checkpoint checkpoint() {
        std::array<unsigned char, 64> blocks[2]{};
        bool valid[2]{};
        for (unsigned i = 0; i < 2; ++i) {
            read_super(i, blocks[i]);
            valid[i] =
                (!std::memcmp(blocks[i].data(), "FQROOT04", 8) ||
                 !std::memcmp(blocks[i].data(), "FQROOT05", 8)) &&
                storage::crc32(blocks[i].data(), 60) == storage::get32(blocks[i].data() + 60) &&
                storage::get64(blocks[i].data() + 8) != 0;
        }
        require(valid[0] || valid[1]);
        unsigned selected = !valid[0] ? 1
                                      : (!valid[1] ? 0
                                                   : (storage::get64(blocks[1].data() + 8) >
                                                              storage::get64(blocks[0].data() + 8)
                                                          ? 1
                                                          : 0));
        auto &block = blocks[selected];
        auto offset = storage::get64(block.data() + 16);
        require(offset >= data_start && offset < tail);
        auto sealed = frame(offset, tail);
        require(sealed && sealed->kind == Frame::Kind::seal);
        Bytes payload(sealed->length, '\0');
        file->read(sealed->payload, payload.data(), payload.size());
        wire::Reader reader(payload);
        wire::Field field;
        uint64_t fields[16]{};
        unsigned seen = 0;
        while (reader.next(field)) {
            if (field.number >= 1 && field.number <= 16) {
                require(field.type == wire::Type::Fixed64 &&
                        reader.fixed(fields[field.number - 1], 8));
                seen |= 1u << (field.number - 1);
            } else
                require(reader.skip(field));
        }
        require(reader.done() && (seen & 255) == 255 && fields[8] <= 7 &&
                fields[1] == sealed->version &&
                (fields[0] == 0 || (fields[0] >= data_start && fields[0] < offset)) &&
                storage::get64(block.data() + 24) == sealed->end &&
                storage::get64(block.data() + 32) == sealed->version && fields[7] <= fields[1] &&
                fields[6] == number);
        require(fields[11]
                    ? ((fields[8] & 4) && fields[11] < number && fields[12] == fields[7] &&
                       fields[13] >= data_start && fields[14] <= 1 && fields[15] <= fields[7])
                    : !(fields[12] || fields[13] || fields[14] || fields[15]));
        uint64_t history_frame = 0, history_start = 0;
        if (!std::memcmp(block.data(), "FQROOT05", 8)) {
            history_frame = storage::get64(block.data() + 48);
            history_start = storage::get64(block.data() + 40);
            require(history_frame >= data_start && history_frame < history_start &&
                    history_start <= sealed->end);
            auto boundary = frame(history_frame, history_start);
            require(boundary && boundary->kind == Frame::Kind::seal &&
                        boundary->version == fields[7] && boundary->end == history_start);
        }
        return {fields[0],  fields[1],  sealed->end, storage::get64(block.data() + 8),
                fields[2],  fields[3],  fields[4],   fields[5],
                fields[7],  fields[8],  fields[9],   fields[10],
                fields[11], fields[12], fields[13],  fields[14],
                fields[15], history_frame, history_start};
    }
    Bytes checkpoint_payload(const Checkpoint &checkpoint) const {
        wire::Writer payload;
        uint64_t fields[]{checkpoint.root,
                          checkpoint.version,
                          checkpoint.entries,
                          checkpoint.user_keys,
                          checkpoint.live_bytes,
                          checkpoint.time,
                          number,
                          checkpoint.floor,
                          checkpoint.features,
                          checkpoint.origin_hi,
                          checkpoint.origin_lo};
        for (unsigned i = 0; i < 11; ++i)
            payload.fixed64_field(i + 1, fields[i]);
        if (checkpoint.parent) {
            uint64_t parent[]{checkpoint.parent, checkpoint.parent_version, checkpoint.parent_limit,
                              checkpoint.parent_legacy, checkpoint.retained_floor};
            for (unsigned i = 0; i < 5; ++i)
                payload.fixed64_field(12 + i, parent[i]);
        }
        return payload.take();
    }
    bool has_reclaim_floor(const Checkpoint &checkpoint) {
        // A logical version alone is not a reclaim barrier. Both physical
        // roots must already seal this exact floor, including after reopen.
        if (checkpoint.sequence < 2 || checkpoint.floor != checkpoint.version ||
            checkpoint.end != tail || checkpoint.parent || !checkpoint.history_frame ||
            !checkpoint.history_start)
            return false;
        const auto expected = checkpoint_payload(checkpoint);
        try {
            for (unsigned slot = 0; slot < 2; ++slot) {
                std::array<unsigned char, 64> block{};
                read_super(slot, block);
                const bool latest = slot == checkpoint.sequence % 2;
                if (std::memcmp(block.data(), "FQROOT05", 8) ||
                    storage::crc32(block.data(), 60) != storage::get32(block.data() + 60) ||
                    storage::get64(block.data() + 8) != checkpoint.sequence - !latest ||
                    storage::get64(block.data() + 32) != checkpoint.version ||
                    storage::get64(block.data() + 40) != checkpoint.history_start ||
                    storage::get64(block.data() + 48) != checkpoint.history_frame)
                    return false;
                const auto offset = storage::get64(block.data() + 16);
                if (offset < data_start || offset >= tail ||
                    (!latest && offset != checkpoint.history_frame))
                    return false;
                const auto sealed = frame(offset, tail);
                if (!sealed || sealed->kind != Frame::Kind::seal ||
                    sealed->version != checkpoint.version || sealed->length != expected.size() ||
                    sealed->end != storage::get64(block.data() + 24) ||
                    sealed->end != (latest ? checkpoint.end : checkpoint.history_start))
                    return false;
                Bytes actual(sealed->length, '\0');
                file->read(sealed->payload, actual.data(), actual.size());
                if (actual != expected)
                    return false;
            }
        } catch (const Error &error) {
            // An unusable alternate root needs a fresh pair, never permission
            // to reclaim. Real I/O/quota failures still propagate to callers.
            if (error.status != FQDB_CORRUPT)
                throw;
            return false;
        }
        return true;
    }
    void seal_payload(Checkpoint &checkpoint, std::string_view payload, bool history_boundary = false) {
        require(checkpoint.sequence != UINT64_MAX, FQDB_LIMIT);
        IOWritePermit permit(file->reservation_bytes(tail,payload.size() + 56) + 64);
        auto offset = append(seal_magic, checkpoint.version, payload);
        if (history_boundary) {
            checkpoint.history_frame = offset;
            checkpoint.history_start = tail;
        }
        try {
            file->sync(); // Referenced pages and WAL must be durable before this pointer.
            std::array<unsigned char, 64> block{};
            std::memcpy(block.data(), checkpoint.history_start ? "FQROOT05" : "FQROOT04", 8);
            storage::put64(block.data() + 8, ++checkpoint.sequence);
            storage::put64(block.data() + 16, offset);
            storage::put64(block.data() + 24, tail);
            storage::put64(block.data() + 32, checkpoint.version);
            storage::put64(block.data() + 40, checkpoint.history_start);
            storage::put64(block.data() + 48, checkpoint.history_frame);
            storage::put32(block.data() + 60, storage::crc32(block.data(), 60));
            file->write(4096 + (checkpoint.sequence % 2) * 4096, block.data(), block.size());
            file->sync();
            checkpoint.end = tail;
        } catch (...) {
            write_failed = true;
            throw;
        }
    }
    void seal(Checkpoint &checkpoint) {
        require(checkpoint.sequence != UINT64_MAX, FQDB_LIMIT);
        auto payload = checkpoint_payload(checkpoint);
        seal_payload(checkpoint, payload);
    }
};
inline std::shared_ptr<const Page> Generation::page(uint64_t offset, uint64_t limit, bool cached) {
    require(offset >= data_start && offset < limit);
    if (auto found = cached ? pages.get(offset) : std::shared_ptr<const Page>{}) {
        require(found->frame_end <= limit);
        return found;
    }
    auto record = frame(offset, limit, false);
    require(record && (record->kind == Frame::Kind::leaf || record->kind == Frame::Kind::branch));
    Bytes payload(record->length, '\0');
    file->read(record->payload, payload.data(), payload.size());
    require(storage::crc32(payload.data(), payload.size()) == record->crc);
    auto result = std::make_shared<Page>(Page::decode(std::move(payload)));
    require(result->leaf == (record->kind == Frame::Kind::leaf) &&
            result->maximum <= record->version);
    result->frame_offset = record->offset;
    result->frame_end = record->end;
    result->frame_version = record->version;
    try {
        if (cached)
            pages.put(offset, result, result->allocation());
    } catch (const std::bad_alloc &) {
    }
    return result;
}
// Small buffered parsing skips opaque values by offset instead of allocating a
// transaction-sized recovery buffer. Payload CRC is checked independently.
class Input {
    SharedFile &file_;
    uint64_t position_, end_, buffered_at_ = 0;
    size_t buffered_ = 0;
    std::array<unsigned char, 16384> buffer_{};

  public:
    Input(SharedFile &file, uint64_t begin, uint64_t end)
        : file_(file), position_(begin), end_(end) {
        require(begin <= end);
    }
    uint64_t position() const noexcept {
        return position_;
    }
    uint64_t end() const noexcept {
        return end_;
    }
    unsigned char byte() {
        require(position_ < end_);
        if (position_ < buffered_at_ || position_ - buffered_at_ >= buffered_) {
            buffered_at_ = position_;
            buffered_ = static_cast<size_t>(std::min<uint64_t>(buffer_.size(), end_ - position_));
            file_.read(position_, buffer_.data(), buffered_);
        }
        return buffer_[static_cast<size_t>(position_++ - buffered_at_)];
    }
    uint64_t varint() {
        uint64_t result = 0;
        for (unsigned i = 0; i < 10; ++i) {
            auto value = byte();
            require(i != 9 || value <= 1);
            result |= static_cast<uint64_t>(value & 127) << (7 * i);
            if (!(value & 128))
                return result;
        }
        throw Error(FQDB_CORRUPT);
    }
    uint64_t fixed(unsigned size) {
        require(size == 4 || size == 8);
        uint64_t result = 0;
        for (unsigned i = 0; i < size; ++i)
            result |= static_cast<uint64_t>(byte()) << (8 * i);
        return result;
    }
    uint32_t tag() {
        auto result = varint();
        require(result <= UINT32_MAX && (result >> 3) && (result & 7) <= 5);
        return static_cast<uint32_t>(result);
    }
    void skip(uint64_t size) {
        require(size <= end_ - position_);
        position_ += size;
    }
    Bytes bytes(size_t maximum) {
        auto size = varint();
        require(size <= maximum && size <= end_ - position_);
        Bytes result(static_cast<size_t>(size), '\0');
        file_.read(position_, result.data(), result.size());
        position_ += size;
        return result;
    }
    void skip_field(uint32_t field, unsigned depth = 0) {
        auto type = field & 7;
        if (type == 0)
            (void)varint();
        else if (type == 1)
            skip(8);
        else if (type == 2) {
            auto size = varint();
            skip(size);
        } else if (type == 5)
            skip(4);
        else if (type == 3 && depth < 64) {
            for (;;) {
                auto nested = tag();
                if ((nested & 7) == 4) {
                    require((nested >> 3) == (field >> 3));
                    break;
                }
                skip_field(nested, depth + 1);
            }
        } else
            throw Error(FQDB_CORRUPT);
    }
};
} // namespace fengqudb::paged

#pragma once
#include "generation.hpp"

namespace fengqudb::paged {
// One maintenance output at a time is allowed per Store. Persist the intent
// BEFORE creating its private sibling directory, so recovery never has to scan
// unrelated backup folders or guess ownership from a .tmp suffix.
struct OutputIntent {
    Uuid source{}, token{};
    std::string destination, temporary;
};
constexpr size_t output_payload_bytes = 2048;
inline bool plain_entry(const std::filesystem::path &path, bool directory) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error || std::filesystem::is_symlink(status) ||
        (directory ? !std::filesystem::is_directory(status)
                   : !std::filesystem::is_regular_file(status)))
        return false;
#ifdef _WIN32
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
        return false;
#endif
    return true;
}
inline std::array<unsigned char, 64> output_identity(const OutputIntent &intent,
                                                    std::string_view magic) {
    std::array<unsigned char, 64> bytes{};
    std::memcpy(bytes.data(), magic.data(), 8);
    std::memcpy(bytes.data() + 16, intent.source.data(), 16);
    std::memcpy(bytes.data() + 32, intent.token.data(), 16);
    storage::put32(bytes.data() + 60, storage::crc32(bytes.data(), 60));
    return bytes;
}
inline bool output_marker(const std::filesystem::path &path, const OutputIntent &intent) {
    if (!plain_entry(path, false))
        return false;
    NativeFile file;
    file.open(path.u8string(), false, true, true);
    if (file.size() != 64)
        return false;
    std::array<unsigned char, 64> bytes{};
    file.read(0, bytes.data(), bytes.size());
    return bytes == output_identity(intent, "FQOWN001");
}
inline bool managed_output_name(std::string_view name) {
    if (name == "LOCK" || name == "CURRENT" || name == "WRITE" || name == "OWNER" ||
        name == "CURRENT.tmp" || name == "WRITE.tmp" || name == "RECOVER.tmp" || name == "SEGMENT.tmp")
        return true;
    if (generation_filename(name))
        return true;
    if (name.size() == 21 && name.substr(17) == ".blk")
        return generation_filename(std::string(name.substr(0, 17)) + ".fdb");
    if (name.size() == 30 && name[0] == 's' && name[17] == '_' && name.substr(26) == ".fdb") {
        for (size_t i = 1; i < 26; ++i)
            if (i != 17 && !((name[i] >= '0' && name[i] <= '9') ||
                             (name[i] >= 'a' && name[i] <= 'f')))
                return false;
        return true;
    }
    if (name.size() != 21 || name.substr(17) != ".tmp" ||
        std::string_view("mwrx").find(name[0]) == std::string_view::npos)
        return false;
    for (size_t i = 1; i < 17; ++i)
        if (!((name[i] >= '0' && name[i] <= '9') || (name[i] >= 'a' && name[i] <= 'f')))
            return false;
    return true;
}
inline bool discard_partial_owner(const std::filesystem::path &directory,
                                   const OutputIntent &intent) {
    // Before OWNER is fully written/synced, no LOCK or data may be created.
    // This handles termination inside that first write without accepting a
    // foreign marker or touching any directory that contains actual data.
    size_t entries = 0;
    for (const auto &entry : std::filesystem::directory_iterator(directory))
        if (++entries > 1 || entry.path().filename() != "OWNER" || !plain_entry(entry.path(), false))
            return false;
    if (entries != 1) return false;
    const auto path = directory / "OWNER";
    NativeFile file; file.open(path.u8string(), false, true, true); file.lock();
    const auto size = file.size();
    if (size >= 64) return false;
    auto expected = output_identity(intent, "FQOWN001");
    std::array<unsigned char, 64> actual{};
    file.read(0, actual.data(), static_cast<size_t>(size));
    if (std::memcmp(actual.data(), expected.data(), static_cast<size_t>(size))) return false;
    remove_owned(path.u8string()); file.retire();
    std::error_code error;
    return std::filesystem::remove(directory, error) && !error;
}
inline bool recover_output(const std::string &directory, const Uuid &source) noexcept {
    // Cleanup must still work after a caller exhausts its maintenance quota.
    auto *quota = storage::io_quota;
    storage::io_quota = nullptr;
    struct RestoreQuota {
        storage::IOQuota *value;
        ~RestoreQuota() { storage::io_quota = value; }
    } restore{quota};
    try {
        const auto journal_path = std::filesystem::u8path(join(directory, "OUTPUT"));
        std::error_code error;
        if (!std::filesystem::exists(journal_path, error))
            return !error;
        if (!plain_entry(journal_path, false))
            return false;
        OutputIntent intent;
        {
            NativeFile file;
            file.open(journal_path.u8string(), false, true, true);
            file.lock();
            const auto size = file.size();
            // No directory can have been created before the complete intent
            // was synced. A torn creation is disposable internal metadata.
            if (size < 64) {
                file.retire();
                remove_owned(journal_path.u8string());
                return !std::filesystem::exists(journal_path);
            }
            if (size > 8192)
                return false;
            std::array<unsigned char, 64> bytes{};
            file.read(0, bytes.data(), bytes.size());
            if (std::memcmp(bytes.data(), "FQOUT001", 8) ||
                std::memcmp(bytes.data() + 16, source.data(), 16) ||
                storage::get32(bytes.data() + 60) != storage::crc32(bytes.data(), 60))
                return false;
            if (size != 64 + output_payload_bytes) {
                file.retire();
                remove_owned(journal_path.u8string());
                return !std::filesystem::exists(journal_path);
            }
            intent.source = source;
            std::memcpy(intent.token.data(), bytes.data() + 32, 16);
            const auto length = storage::get32(bytes.data() + 8);
            if (!length || length > output_payload_bytes) return false;
            intent.destination.resize(length);
            file.read(64, intent.destination.data(), intent.destination.size());
            if (intent.destination.empty() || intent.destination.find('\0') != std::string::npos ||
                storage::get32(bytes.data() + 56) !=
                    storage::crc32(intent.destination.data(), intent.destination.size()))
                return false;
        }
        const auto destination = std::filesystem::u8path(intent.destination);
        if (!destination.is_absolute() || destination.lexically_normal() != destination)
            return false;
        const auto parent = destination.parent_path();
        // A changed/symlinked parent is not evidence that the old output is ours.
        if (std::filesystem::weakly_canonical(parent, error) != parent || error)
            return false;
        const auto staging = parent / temporary_name('t', intent.token);
        if (std::filesystem::exists(staging, error)) {
            if (error || !plain_entry(staging, true))
                return false;
            if (std::filesystem::is_empty(staging, error) && !error) {
                // Covers the mkdir -> marker crash boundary; remove is atomic
                // and fails if another file appeared, rather than recursing.
                if (!std::filesystem::remove(staging, error) || error)
                    return false;
            } else {
                if (error)
                    return false;
                if (!output_marker(staging / "OWNER", intent)) {
                    if (!discard_partial_owner(staging, intent)) return false;
                } else {
                    storage::File lease;
                    if (lease.open((staging / "LOCK").u8string().c_str()) != FQDB_OK)
                        return false;
                    std::vector<std::filesystem::path> files;
                    for (const auto &entry : std::filesystem::directory_iterator(staging)) {
                        if (files.size() >= 4096 || !plain_entry(entry.path(), false) ||
                            !managed_output_name(entry.path().filename().u8string()))
                            return false;
                        files.push_back(entry.path());
                    }
                    // Validate everything before removing anything. Unknown files,
                    // links, and nested directories preserve the whole output.
                    for (const auto &path : files)
                        if (path.filename() != "OWNER" && path.filename() != "LOCK") {
                            if (!std::filesystem::remove(path, error) || error)
                                return false;
                        }
                    lease.close();
                    remove_owned((staging / "LOCK").u8string());
                    if (!std::filesystem::remove(staging / "OWNER", error) || error ||
                        !std::filesystem::remove(staging, error) || error)
                        return false;
                }
            }
        } else if (error)
            return false;
        // A crash after rename must NEVER turn a published backup into trash.
        // Only remove its disposable ownership marker, if it still matches.
        if (plain_entry(destination, true) && output_marker(destination / "OWNER", intent))
            remove_owned((destination / "OWNER").u8string());
        if (!storage::sync_parent(staging.u8string().c_str())) return false;
        if (plain_entry(destination, true) &&
            !storage::sync_parent((destination / "OWNER").u8string().c_str())) return false;
        remove_owned(journal_path.u8string());
        if (!std::filesystem::exists(journal_path, error) && !error) {
            (void)storage::sync_parent(journal_path.u8string().c_str());
            return true;
        }
    } catch (...) {
    }
    return false;
}
inline OutputIntent register_output(const std::string &directory, const Uuid &source,
                                    const std::string &destination) {
    require(recover_output(directory, source), FQDB_LOCKED);
    const auto resolved = std::filesystem::weakly_canonical(std::filesystem::u8path(destination));
    OutputIntent intent{source, new_uuid(), directory_path(resolved.u8string()), {}};
    const auto path = std::filesystem::u8path(intent.destination);
    intent.temporary = directory_path((path.parent_path() / temporary_name('t', intent.token)).u8string());
    auto bytes = output_identity(intent, "FQOUT001");
    require(intent.destination.size() <= output_payload_bytes, FQDB_LIMIT);
    std::array<char, output_payload_bytes> payload{};
    std::memcpy(payload.data(), intent.destination.data(), intent.destination.size());
    storage::put32(bytes.data() + 8, static_cast<uint32_t>(intent.destination.size()));
    storage::put32(bytes.data() + 56,
                   storage::crc32(intent.destination.data(), intent.destination.size()));
    storage::put32(bytes.data() + 60, storage::crc32(bytes.data(), 60));
    const auto journal = join(directory, "OUTPUT");
    NativeFile file;
    try {
        // Fixed-size control metadata makes exact maintenance budgets stable
        // across destination names of different lengths.
        IOWritePermit permit(bytes.size() + payload.size());
        file.open(journal, true);
        file.write(0, bytes.data(), bytes.size());
        file.write(bytes.size(), payload.data(), payload.size());
        file.sync();
        require(storage::sync_parent(journal.c_str()), FQDB_IO_ERROR);
        return intent;
    } catch (...) {
        if (file.exclusively_created()) {
            file.retire();
            remove_owned(journal);
        }
        throw;
    }
}
inline void reclaim_fixed_temporaries(const std::string &directory) noexcept {
    // These exact names are private metadata/scratch paths. CURRENT only ever
    // selects canonical .fdb names; none can contain authoritative user data.
    // The caller already owns the Store directory lease. Retain links and any
    // independently locked file; never use a suffix wildcard or recurse.
    for (const auto *name : {"CURRENT.tmp", "WRITE.tmp", "RECOVER.tmp", "SEGMENT.tmp"}) {
        try {
            const auto path = std::filesystem::u8path(join(directory, name));
            if (!plain_entry(path, false)) continue;
            NativeFile file; file.open(path.u8string(), false, true, true); file.lock();
            remove_owned(path.u8string());
        } catch (...) { }
    }
}
inline void reclaim_store_temporaries(const std::string &directory, const Uuid &uuid,
                                      uint64_t selected, const std::vector<uint64_t> &keep) noexcept {
    // Reserved names + format/identity + an exclusive file lease are required.
    // Ordinary files, links, unknown formats and actively held files are kept.
    try {
        size_t visited = 0;
        for (const auto &entry : std::filesystem::directory_iterator(std::filesystem::u8path(directory))) {
            if (++visited > 4096) break;
            const auto name = entry.path().filename().u8string();
            if (name.size() != 21 || name.substr(17) != ".tmp" ||
                std::string_view("mwrx").find(name[0]) == std::string_view::npos ||
                !managed_output_name(name) || !plain_entry(entry.path(), false)) continue;
            try {
                NativeFile file;
                file.open(entry.path().u8string(), false, true, true);
                file.lock();
                const auto size = file.size();
                std::array<unsigned char, 64> header{};
                file.read(0, header.data(), static_cast<size_t>(std::min<uint64_t>(size, header.size())));
                bool owned = false;
                if (name[0] == 'w' && size == 40)
                    owned = !std::memcmp(header.data(), "FQWRITE1", 8) &&
                            storage::get32(header.data() + 8) == 1 &&
                            storage::get32(header.data() + 36) == storage::crc32(header.data(), 36);
                else if (name[0] == 'r' && size >= 64) {
                    Uuid scratch{}; std::memcpy(scratch.data(), header.data() + 16, 16);
                    owned = !std::memcmp(header.data(), "FQDATA04", 8) &&
                            storage::get32(header.data() + 60) == storage::crc32(header.data(), 60) &&
                            name == temporary_name('r', scratch);
                } else if (name[0] == 'm' && size >= 32 && size <= 4096) {
                    if (!std::memcmp(header.data(), "FQCUR004", 8) &&
                        storage::get32(header.data() + 28) == storage::crc32(header.data(), 28) &&
                        storage::get32(header.data() + 12) == size - 32) {
                        Bytes payload(static_cast<size_t>(size - 32), '\0');
                        file.read(32, payload.data(), payload.size());
                        if (storage::get32(header.data() + 24) == storage::crc32(payload.data(), payload.size())) {
                            wire::Reader reader(payload); wire::Field field;
                            while (reader.next(field)) {
                                if (field.number == 1 && field.type == wire::Type::Bytes) {
                                    std::string_view identity;
                                    if (reader.bytes(identity) && identity.size() == 16 &&
                                        !std::memcmp(identity.data(), uuid.data(), 16)) owned = true;
                                } else if (!reader.skip(field)) break;
                            }
                            owned = owned && reader.done();
                        }
                    }
                } else if (name[0] == 'x') {
                    uint64_t token = 0;
                    for (size_t i = 1; i < 17; ++i)
                        token = (token << 4) | unsigned(name[i] <= '9' ? name[i] - '0' : name[i] - 'a' + 10);
                    const auto number = token ^ storage::get64(uuid.data());
                    if (number) {
                        if (number == selected || std::find(keep.begin(), keep.end(), number) != keep.end()) owned = true;
                        else {
                            Generation original(0);
                            original.open(join(directory, generation_name(uuid, number)), uuid, number, true);
                            owned = true;
                        }
                    }
                }
                if (owned) {
                    file.retire();
                    remove_owned(entry.path().u8string());
                }
            } catch (...) { }
        }
    } catch (...) { }
}
} // namespace fengqudb::paged

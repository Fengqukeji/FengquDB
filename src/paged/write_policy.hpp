#pragma once
#include "generation.hpp"

namespace fengqudb::paged {
inline fqdb_store_write_options write_defaults() {
    return {sizeof(fqdb_store_write_options), 1, 1, 50, 1048576};
}
inline bool valid_write_policy(const fqdb_store_write_options &p) {
    return p.struct_size >= sizeof(p) && p.abi_version == 1 && p.mode <= 1 && p.period_ms >= 1 &&
           p.period_ms <= 60000 && p.threshold_bytes >= 1 &&
           p.threshold_bytes <= 64ull * 1024 * 1024;
}
inline fqdb_store_write_options load_write_policy(const std::string &directory,
                                                  bool *saved = nullptr) {
    auto path = join(directory, "WRITE");
    std::error_code error;
    const bool exists = std::filesystem::exists(std::filesystem::u8path(path), error);
    require(!error, FQDB_IO_ERROR);
    if (saved)
        *saved = false;
    if (!exists)
        return write_defaults();
    NativeFile file;
    file.open(path, false, true, true);
    std::array<unsigned char, 40> bytes{};
    require(file.size() == bytes.size());
    file.read(0, bytes.data(), bytes.size());
    require(!std::memcmp(bytes.data(), "FQWRITE1", 8) && storage::get32(bytes.data() + 8) == 1 &&
            storage::get32(bytes.data() + 36) == storage::crc32(bytes.data(), 36));
    auto p = write_defaults();
    p.mode = storage::get32(bytes.data() + 12);
    p.period_ms = storage::get32(bytes.data() + 16);
    require(storage::get32(bytes.data() + 20) == 0 && storage::get32(bytes.data() + 32) == 0);
    p.threshold_bytes = storage::get64(bytes.data() + 24);
    require(valid_write_policy(p));
    if (saved)
        *saved = true;
    return p;
}
inline fqdb_status save_write_policy(const std::string &directory,
                                     const fqdb_store_write_options &p) {
    const auto target = join(directory, "WRITE");
    const auto temporary = join(directory, temporary_name('w', new_uuid()));
    bool created = false;
    try {
        std::array<unsigned char, 40> bytes{};
        std::memcpy(bytes.data(), "FQWRITE1", 8);
        storage::put32(bytes.data() + 8, 1);
        storage::put32(bytes.data() + 12, p.mode);
        storage::put32(bytes.data() + 16, p.period_ms);
        storage::put64(bytes.data() + 24, p.threshold_bytes);
        storage::put32(bytes.data() + 36, storage::crc32(bytes.data(), 36));
        NativeFile file;
        file.open(temporary, true);
        created = file.exclusively_created();
        require(created, FQDB_IO_ERROR);
        file.write(0, bytes.data(), bytes.size());
        file.sync();
        file.retire();
        auto result = publish_manifest(temporary, target);
        remove_owned(temporary);
        return result;
    } catch (...) {
        if (created)
            remove_owned(temporary);
        throw;
    }
}
} // namespace fengqudb::paged

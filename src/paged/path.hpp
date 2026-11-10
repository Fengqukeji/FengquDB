#pragma once
#include "io.hpp"

namespace fengqudb::paged {
// Leave room for a fixed short internal name. Windows counts UTF-16 units;
// POSIX counts UTF-8 bytes. The POSIX bound also fits Darwin's PATH_MAX.
inline void check_path(const std::filesystem::path &path, size_t reserve = 0) {
#ifdef _WIN32
    constexpr size_t limit = 240;
#else
    constexpr size_t limit = 960;
#endif
    require(reserve < limit && path.native().size() < limit - reserve, FQDB_LIMIT);
    // Check every caller-supplied component before mkdir can create part of
    // the root. A short total path can still exceed POSIX NAME_MAX.
    for (const auto &component : path.relative_path())
        require(component.native().size() <= 255, FQDB_LIMIT);
}
inline std::string directory_path(const std::string &text, bool container = false) {
    require(!text.empty() && text.find('\0') == std::string::npos, FQDB_INVALID_ARGUMENT);
    auto path = std::filesystem::absolute(std::filesystem::u8path(text)).lexically_normal();
    while (path != path.root_path() && path.filename().empty())
        path = path.parent_path();
    require(container || path != path.root_path(), FQDB_INVALID_ARGUMENT);
    check_path(path, 24);
    return path.u8string();
}
inline std::string database_path(const std::string &root, std::string_view name) {
    require(!name.empty() && name.size() <= 32, FQDB_INVALID_ARGUMENT);
    std::string upper;
    for (unsigned char ch : name) {
        require((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
                    ch == '_' || ch == '-',
                FQDB_INVALID_ARGUMENT);
        upper.push_back(ch >= 'a' && ch <= 'z' ? static_cast<char>(ch - 'a' + 'A') : ch);
    }
    require(upper != "CON" && upper != "PRN" && upper != "AUX" && upper != "NUL" &&
                !(upper.size() == 4 &&
                  (upper.substr(0, 3) == "COM" || upper.substr(0, 3) == "LPT") && upper[3] >= '1' &&
                  upper[3] <= '9'),
            FQDB_INVALID_ARGUMENT);
    return directory_path(
        (std::filesystem::u8path(directory_path(root, true)) / std::filesystem::u8path(name))
            .u8string());
}
} // namespace fengqudb::paged

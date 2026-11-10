#pragma once
#include "fengqudb_store.hpp"
// Existing engine regressions intentionally exercise the original strict path.
// The default-on periodic path has its own public-API/fault/lifetime tests.
namespace test_support {
inline void strict(fengqudb::Store &store) {
    auto options = store.write_options();
    if (options.mode == fengqudb::StoreWriteMode::Performance) {
        options.mode = fengqudb::StoreWriteMode::Reliable;
        store.configure_write(options);
    }
}
class StrictStore : public fengqudb::Store {
  public:
    explicit StrictStore(std::string_view directory, const fengqudb::StoreOptions &options = {})
        : fengqudb::Store(directory, options) {
        strict(*this);
    }
    explicit StrictStore(fengqudb::Store &&store) : fengqudb::Store(std::move(store)) {
        strict(*this);
    }
    static StrictStore open_at(const std::string &root, const std::string &name,
                               const fengqudb::StoreOptions &options = {}) {
        return StrictStore(fengqudb::Store::open_at(root, name, options));
    }
};
inline fqdb_status open_strict(const char *path, const fqdb_store_options *options,
                               fqdb_store **out) {
    auto status = fqdb_store_open(path, options, out);
    if (status || !out || !*out)
        return status;
    fqdb_store_write_options p{sizeof(p), 1, 0, 100, 1048576};
    status = fqdb_store_write_configure(*out, &p);
    if (status) {
        fqdb_store_close(*out);
        *out = nullptr;
    }
    return status;
}
} // namespace test_support

#include <fengqudb_store.h>
#include <cstring>

// A full device/simulator link catches unavailable libc++/Darwin symbols and
// missing C exports; producing an archive alone cannot detect those failures.
int main() {
    fqdb_store *store = nullptr;
    auto status = fqdb_store_open_at("save", "main", nullptr, &store);
    if (status != FQDB_OK) return status;
    fqdb_store_write_options options{sizeof(options), 1, FQDB_WRITE_RELIABLE, 50, 1048576};
    status = fqdb_store_write_configure(store, &options);
    if (status == FQDB_OK) {
        fqdb_store_atomic_info result{};
        status = fqdb_store_put(store, {"name", 4}, {"Alice", 5}, 0, &result, 1);
    }
    fqdb_store_close(store);
    if (status != FQDB_OK) return status;
    store = nullptr;
    status = fqdb_store_open_at("save", "main", nullptr, &store);
    if (status != FQDB_OK) return status;
    fqdb_buffer value{};
    status = fqdb_store_get(store, {"name", 4}, &value);
    if (status == FQDB_OK && (value.size != 5 || std::memcmp(value.data, "Alice", 5) != 0))
        status = FQDB_INTERNAL_ERROR;
    fqdb_buffer_free(&value);
    fqdb_store_close(store);
    return status;
}

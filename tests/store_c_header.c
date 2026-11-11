#include "fengqudb_store.h"
int fqdb_store_c_header_smoke(void) {
    fqdb_store *store = (fqdb_store *)1;
    fqdb_store_value *value = (fqdb_store_value *)1;
    fqdb_store_watch *watch = (fqdb_store_watch *)1;
    fqdb_slice empty = {0, 0};
    if (fqdb_store_open(0, 0, &store) != FQDB_INVALID_ARGUMENT || store != 0)
        return 1;
    if (fqdb_store_get_view(0, empty, &value) != FQDB_INVALID_ARGUMENT || value != 0)
        return 2;
    if (fqdb_store_value_data(0).size != 0)
        return 3;
    if (fqdb_store_watch_open(0, 0, empty, 0, &watch) != FQDB_INVALID_ARGUMENT || watch != 0 ||
        fqdb_store_change_batch_data(0) != 0)
        return 5;
    {
        fqdb_store_expiry_info info = {1, 1, 1, 1, 1, 1};
        if (fqdb_store_purge_expired(0, 0, &info) != FQDB_INVALID_ARGUMENT || info.version != 0)
            return 4;
    }
    {
        fqdb_store_i64_field_update item = {{0, 0}, 0, 1, 0, INT64_MAX};
        fqdb_store_atomic_info info = {1, 1, 1, 1, 1};
        if (fqdb_store_add_i64_fields(0, &item, 1, 0, &info, 0) != FQDB_INVALID_ARGUMENT ||
            info.version != 0)
            return 6;
    }
    {
        fqdb_buffer bytes = {(void *)1, 1};
        fqdb_store_reader *reader = (fqdb_store_reader *)1;
        if (fqdb_store_project(0, empty, 0, 0, &bytes) != FQDB_INVALID_ARGUMENT ||
            bytes.data != 0 || bytes.size != 0 ||
            fqdb_store_project_reader_create(0, 0, 0, 0, 0, &reader) != FQDB_INVALID_ARGUMENT ||
            reader != 0)
            return 7;
    }
    {
        fqdb_buffer bytes = {(void *)1, 1};
        if (fqdb_store_select(0, empty, &bytes) != FQDB_INVALID_ARGUMENT || bytes.data != 0 ||
            bytes.size != 0)
            return 8;
    }
    return 0;
}

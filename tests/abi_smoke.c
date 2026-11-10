#include "fengqudb_store.h"
#include <stddef.h>
#include <string.h>
int main(void) {
    fqdb_buffer out = {NULL, 0};
    const unsigned char bytes[] = {0, 255, 42};
    if (fqdb_abi_version() != FQDB_ABI_VERSION || sizeof(fqdb_slice) != 16)
        return 1;
    if (fqdb_buffer_copy((fqdb_slice){bytes, sizeof(bytes)}, &out) != FQDB_OK ||
        out.size != sizeof(bytes) || memcmp(out.data, bytes, sizeof(bytes)))
        return 2;
    fqdb_buffer_free(&out);
    if (out.data || out.size || fqdb_buffer_copy((fqdb_slice){NULL, 1}, &out) != FQDB_INVALID_ARGUMENT)
        return 3;
    if (fqdb_store_get(NULL, (fqdb_slice){NULL, 0}, &out) != FQDB_INVALID_ARGUMENT)
        return 4;
    fqdb_store_close(NULL);
    return 0;
}

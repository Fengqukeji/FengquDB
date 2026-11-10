#include <fengqudb_store.h>
#include <stdio.h>
#include <string.h>
static fqdb_slice bytes(const char *text) {
    return (fqdb_slice){text, strlen(text)};
}
int main(int argc, char **argv) {
    fqdb_store *db = NULL;
    fqdb_store_txn *tx = NULL;
    fqdb_buffer value = {NULL, 0};
    uint64_t version = 0;
    fqdb_status status = fqdb_store_open(argc > 1 ? argv[1] : "example-c-db", NULL, &db);
    if (!status)
        status = fqdb_store_txn_begin(db, 0, &tx);
    if (!status)
        status = fqdb_store_txn_put(tx, bytes("player/42"), bytes("protobuf bytes"), 0);
    fqdb_store_commit *pending = NULL;
    if (!status)
        status = fqdb_store_txn_submit(tx, &pending, 0);
    if (!status) {
        fqdb_status outcome = FQDB_OK;
        status = fqdb_store_commit_wait(pending, 0, &outcome, &version);
        if (!status)
            status = outcome;
    }
    fqdb_store_commit_close(pending);
    fqdb_store_txn_close(tx);
    if (!status)
        status = fqdb_store_get(db, bytes("player/42"), &value);
    if (!status)
        printf("%.*s\n", (int)value.size, (const char *)value.data);
    else
        fprintf(stderr, "%s\n", fqdb_status_message(status));
    fqdb_buffer_free(&value);
    fqdb_store_close(db);
    return status ? 1 : 0;
}

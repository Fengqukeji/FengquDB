#include "paged/table.hpp"
#include "paged/wal.hpp"
#include <iostream>
#include <map>
#include <random>

using namespace fengqudb::paged;
namespace storage = fengqudb::storage;
static void check(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
template <class F> static void corrupt(F &&function) {
    try {
        function();
    } catch (const Error &error) {
        check(error.status == FQDB_CORRUPT, "unexpected error status");
        return;
    }
    throw std::runtime_error("corruption was accepted");
}
static Value bytes(Bytes value) {
    return std::make_shared<const Data>(std::move(value));
}
static bool same(const Ref &a, const Ref &b);
static void buffered_wals() {
    for (bool segmented : {false, true}) {
        const auto id = new_uuid();
        const auto directory = std::filesystem::temp_directory_path() /
                               ("fqdb-wal-buffer-" + hex(id));
        check(std::filesystem::create_directory(directory), "owned WAL fixture");
        const auto path = join(directory.u8string(), generation_name(id, 1));
        auto generation = std::make_shared<Generation>(0);
        generation->create(path, id, 1, segmented ? 65536 : 0);
        uint64_t version = 0;
        for (const auto size : {size_t{0}, size_t{511}, size_t{512}, size_t{4096},
                                size_t{16300}, size_t{16384}, size_t{131072}}) {
            for (const auto count : {1u, 32u}) {
                WalBatch wal(++version, 1234);
                std::vector<Bytes> expected;
                for (unsigned i = 0; i < count; ++i) {
                    expected.emplace_back(size, static_cast<char>('a' + i % 26));
                    wal.add(OpKind::put, "key/" + std::to_string(1000 + i), bytes(expected.back()));
                }
                const auto offset = generation->tail;
                const auto writes = generation->file->writes.load();
                auto refs = wal.append(*generation);
                const auto frame = generation->frame(offset, generation->tail); // Full CRC scan.
                check(bool(frame), "buffered WAL frame checksum");
                if (size == 4096 && count == 1)
                    check(generation->file->writes.load() == writes + 1,
                          "4 KiB WAL was split into multiple logical writes");
                unsigned decoded = 0;
                read_wal(*generation, *frame, [&](const WalOperation &op) {
                    check(same(op.result, refs[decoded]), "buffered WAL reference offset");
                    check(*generation->value(op.result, generation->tail) == expected[decoded],
                          "buffered WAL payload differs from model");
                    ++decoded;
                });
                check(decoded == count, "buffered WAL record count");
            }
        }
        generation->file->sync();
        generation->file->remove_files(path);
        generation.reset();
        check(std::filesystem::remove(directory), "empty WAL fixture");
    }
}
struct Owned {
    std::string directory;
    std::vector<std::string> files;
    Owned() {
        directory =
            (std::filesystem::temp_directory_path() / ("fqdb-paged-storage-" + hex(new_uuid())))
                .u8string();
        check(std::filesystem::create_directory(std::filesystem::u8path(directory)),
              "owned directory");
    }
    ~Owned() {
        for (const auto &path : files)
            remove_owned(path);
        std::error_code ignored;
        std::filesystem::remove(std::filesystem::u8path(directory), ignored);
    }
};
static bool same(const Ref &a, const Ref &b) {
    return a.version == b.version && a.offset == b.offset && a.size == b.size && a.crc == b.crc &&
           a.depth == b.depth && a.recipe_size == b.recipe_size && a.expires == b.expires;
}
using Model = std::map<Bytes, Ref, fengqudb::detail::ByteLess>;
static void verify(const Table &table, const Model &model) {
    auto cursor = table.cursor();
    for (const auto &item : model) {
        auto slot = cursor.next();
        check(slot && slot->key == item.first && same(slot->value, item.second),
              "ordered disk cursor differs from model");
        auto found = table.find(item.first);
        check(found && same(*found, item.second), "disk point lookup differs from model");
    }
    check(cursor.next() == nullptr, "disk cursor ended late");
    auto reverse = table.cursor({}, true);
    for (auto it = model.rbegin(); it != model.rend(); ++it) {
        auto slot = reverse.next();
        check(slot && slot->key == it->first, "reverse disk cursor differs from model");
    }
    check(reverse.next() == nullptr && !table.find("absent"), "cursor/absent lookup");
    for (uint64_t since : {uint64_t{0}, uint64_t{3}, uint64_t{10}, UINT64_MAX}) {
        auto changed = table.cursor({}, false, since), backward = table.cursor({}, true, since);
        for (const auto &item : model)
            if (item.second.version > since) {
                auto slot = changed.next();
                check(slot && slot->key == item.first && same(slot->value, item.second),
                      "changed cursor model");
            }
        for (auto it = model.rbegin(); it != model.rend(); ++it)
            if (it->second.version > since) {
                auto slot = backward.next();
                check(slot && slot->key == it->first, "changed reverse cursor model");
            }
        check(!changed.next() && !backward.next(), "changed cursor ended late");
    }
    std::vector<Bytes> boundaries{"",      "absent", "k/000",         "k/000/extra",
                                  "k/050", "k/999",  Bytes(1, '\xff')};
    for (const auto &begin : boundaries) {
        auto forward = table.cursor(begin), back = table.cursor(begin, true);
        auto expected = model.lower_bound(begin);
        auto first = forward.next();
        check(expected == model.end() ? first == nullptr : first && first->key == expected->first,
              "cursor seek forward");
        auto upper = model.upper_bound(begin);
        auto last = back.next();
        if (upper == model.begin())
            check(last == nullptr, "cursor seek reverse before minimum");
        else {
            --upper;
            check(last && last->key == upper->first, "cursor seek reverse");
        }
        for (const auto &end : boundaries) {
            uint64_t maximum = 0;
            for (auto it = model.lower_bound(begin);
                 it != model.end() && (end.empty() || compare(it->first, end) < 0); ++it)
                maximum = std::max(maximum, it->second.version);
            check(table.range_max(begin, end) == maximum,
                  "range conflict summary differs from model");
        }
    }
}
int main() {
    try {
        buffered_wals();
        Owned owned;
        auto id = new_uuid();
        auto path = join(owned.directory, generation_name(id, 1));
        auto generation = std::make_shared<Generation>(4 * 1024 * 1024);
        generation->create(path, id, 1);
        owned.files.push_back(path);
        Checkpoint checkpoint;
        generation->seal(checkpoint);
        check(generation->checkpoint().version == 0, "empty checkpoint");
        check(write_current(owned.directory, {id, 1}) == FQDB_OK, "publish current");
        owned.files.push_back(join(owned.directory, "CURRENT"));
        check(read_current(owned.directory).uuid == id, "manifest decode");

        Bytes original(2 * 1024 * 1024, 'a');
        for (size_t i = 0; i < original.size(); ++i)
            original[i] = static_cast<char>(i * 37);
        WalBatch initial(1, 1234);
        initial.add(OpKind::put, "alpha", bytes(original));
        initial.add(OpKind::put, "empty", bytes(""));
        initial.add(OpKind::put, "omega", bytes("tail"));
        auto frame_at = generation->tail;
        auto refs = initial.append(*generation);
        auto frame = generation->frame(frame_at, generation->tail);
        check(frame.has_value(), "complete WAL");
        size_t count = 0;
        check(read_wal(*generation, *frame,
                       [&](const WalOperation &operation) {
                           check(same(operation.result, refs[count++]),
                                 "WAL result reference mismatch");
                       }) == 1234 &&
                  count == 3,
              "streaming WAL parse");
        check(*generation->value(refs[0], generation->tail) == original &&
                  generation->value(refs[1], generation->tail)->empty(),
              "raw disk values");

        // Whole-value private subtraction, then persisted patch recipes. Large
        // deleted spans must not be re-read when the raw base is cold.
        auto base = refs[0];
        Bytes expected = original;
        uint64_t version = 2;
        for (unsigned step = 0; step < 8; ++step, ++version) {
            auto offset = step == 0 ? size_t{1024} : expected.size() / 3;
            auto removed =
                step == 0 ? size_t{1900000} : std::min<size_t>(step * 13, expected.size() - offset);
            auto insert = Bytes(step * 17 + 3, static_cast<char>(step + 11));
            expected.replace(offset, removed, insert);
            auto crc = storage::crc32(expected.data(), expected.size());
            WalBatch patch(version, 1234 + version);
            patch.add(OpKind::patch, "alpha", bytes(insert), base, 5000, offset, removed, crc);
            auto at = generation->tail;
            base = patch.append(*generation)[0];
            auto record = generation->frame(at, generation->tail);
            count = 0;
            read_wal(*generation, *record, [&](const WalOperation &op) {
                check(same(op.result, base), "patch reference mismatch");
                ++count;
            });
            check(count == 1 && base.depth == step + 1, "bounded recipe depth");
            check(*generation->value(base, generation->tail) == expected,
                  "warm cache incremental subtraction");
            auto bad_metadata = base;
            bad_metadata.recipe_size = 1;
            corrupt([&] { generation->value(bad_metadata, generation->tail); });
        }
        // A separate zero-cache generation handle is opened only after releasing
        // the lease; snapshots below keep stable handles once reopened.
        auto limit = generation->tail;
        generation.reset();
        generation = std::make_shared<Generation>(0);
        generation->open(path, id, 1);
        auto before = generation->file->read_bytes.load();
        auto patched = generation->value(base, limit);
        check(*patched == expected, "cold subtraction recipe chain");
        check(generation->file->read_bytes.load() - before < original.size() / 2,
              "cold subtraction read deleted bytes");
        auto retained = patched;
        auto ttl_base = base;
        WalBatch ttl(version++, 2000);
        ttl.add(OpKind::expiry, "alpha", {}, base, 9000);
        base = ttl.append(*generation)[0];
        check(base.offset == ttl_base.offset && base.depth == 8 &&
                  *generation->value(base, generation->tail) == expected,
              "TTL metadata reuses immutable bytes");
        check(*retained == expected && *generation->value(refs[0], generation->tail) == original,
              "old value snapshot was mutated");
        bool limited = false;
        try {
            WalBatch excess(version, 0);
            excess.add(OpKind::patch, "alpha", bytes("x"), base, 0, 0, 0, 0);
        } catch (const Error &error) {
            limited = error.status == FQDB_INVALID_ARGUMENT;
        }
        check(limited, "unbounded patch chain");
        auto invalid = base;
        invalid.recipe_size = 1;
        corrupt([&] { generation->value(invalid, generation->tail); });
        invalid = refs[0];
        invalid.crc ^= 1;
        corrupt([&] { generation->value(invalid, generation->tail); });

        // Model many page splits, replacements and tombstones, retaining every
        // earlier root to detect accidental edits of shared cached pages.
        Model model;
        std::mt19937_64 random(0x718422);
        uint64_t root = 0;
        std::vector<std::pair<uint64_t, Model>> snapshots;
        for (unsigned round = 0; round < 5; ++round, ++version) {
            TableBuilder builder(generation, root, generation->tail, version, 32 * 1024 * 1024,
                                 512);
            for (unsigned n = 0; n < 500; ++n) {
                auto key = Bytes("k/") + std::to_string(1000 + random() % 1500);
                if (n % 13 == 0)
                    key.append(256, static_cast<char>(n));
                auto ref = refs[0];
                ref.version = version;
                if (n % 7 == 0)
                    ref = Ref{version};
                builder.set(key, ref);
                model[key] = ref;
            }
            root = builder.finish();
            verify(Table(generation, root, generation->tail), model);
            snapshots.emplace_back(root, model);
        }
        for (const auto &old : snapshots)
            verify(Table(generation, old.first, generation->tail), old.second);
        checkpoint.root = root;
        checkpoint.version = version;
        checkpoint.entries = model.size();
        checkpoint.time = 3000;
        generation->seal(checkpoint);
        auto recovered = generation->checkpoint();
        check(recovered.root == root && recovered.version == version &&
                  recovered.entries == model.size(),
              "checkpoint decode");
        generation.reset();
        generation = std::make_shared<Generation>(2 * 1024 * 1024);
        generation->open(path, id, 1);
        recovered = generation->checkpoint();
        verify(Table(generation, recovered.root, recovered.end), model);
        // Corrupt the latest pointer block: its independent predecessor remains
        // usable. This models a torn root publication, not a valid data rollback.
        unsigned char bad = 'X';
        generation->file->write(4096 + (checkpoint.sequence % 2) * 4096, &bad, 1);
        check(generation->checkpoint().version == 0, "fallback after torn root block");
        // Recovery may recognize an incomplete tail but must not append behind
        // it until semantic validation and truncation finish.
        auto tail = generation->tail;
        generation->file->write(tail, "partial", 7);
        check(!generation->frame(tail, tail + 7), "incomplete tail classification");
        // Unknown Protobuf groups must have the matching end-group field.
        const unsigned char mismatch[]{0x7b, 0x84, 0x01};
        generation->file->write(tail, mismatch, sizeof(mismatch));
        corrupt([&] {
            Input input(*generation->file, tail, tail + sizeof(mismatch));
            input.skip_field(input.tag());
        });
        bool budget = false;
        try {
            TableBuilder tiny(generation, root, tail, version + 1, 1, 512);
            tiny.set("budget", refs[0]);
        } catch (const Error &error) {
            budget = error.status == FQDB_LIMIT;
        }
        check(budget, "dirty metadata budget");

        DeltaRoot delta;
        Model delta_model;
        for (unsigned round = 0; round < 4; ++round) {
            auto pinned = delta;
            auto pinned_model = delta_model;
            DeltaBuilder edit(delta);
            for (unsigned n = 0; n < 1000; ++n) {
                auto key = Bytes("d/") + std::to_string(random() % 1500);
                Ref ref{round + 1, refs[0].offset, 1};
                edit.set(key, ref);
                delta_model[key] = ref;
            }
            delta = edit.finish();
            for (const auto &item : pinned_model)
                check(same(delta_find(pinned, item.first)->value, item.second),
                      "delta snapshot changed");
            DeltaCursor cursor(delta);
            for (const auto &item : delta_model) {
                auto node = cursor.next();
                check(node && node->key.view() == item.first, "delta cursor model");
            }
            check(!cursor.next(), "delta cursor end");
            for (uint64_t since : {uint64_t{0}, uint64_t{2}, uint64_t{4}}) {
                DeltaCursor changes(delta, {}, false, since), reverse(delta, {}, true, since);
                for (const auto &item : delta_model)
                    if (item.second.version > since) {
                        auto node = changes.next();
                        check(node && node->key.view() == item.first, "changed delta cursor model");
                    }
                for (auto it = delta_model.rbegin(); it != delta_model.rend(); ++it)
                    if (it->second.version > since) {
                        auto node = reverse.next();
                        check(node && node->key.view() == it->first,
                              "changed reverse delta cursor model");
                    }
                check(!changes.next() && !reverse.next(), "changed delta cursor ended late");
            }
            for (auto begin : {"", "d/1", "d/500", "z"})
                for (auto end : {"", "d/300", "d/8", "z"}) {
                    uint64_t maximum = 0;
                    for (auto it = delta_model.lower_bound(begin);
                         it != delta_model.end() && (!*end || compare(it->first, end) < 0); ++it)
                        maximum = std::max(maximum, it->second.version);
                    check(delta_range_max(delta, begin, end) == maximum,
                          "delta range summary model");
                }
        }
        DeltaBuilder empty_keys(delta);
        empty_keys.set("", refs[0]);
        delta = empty_keys.finish();
        DeltaCursor exact_empty(delta, std::string_view{}, true);
        check(exact_empty.next()->key.view().empty() && !exact_empty.next(),
              "reverse seek of empty key");
        std::cout << "PASS paged WAL, subtraction recipes, COW disk table/model, delta snapshots "
                     "and durable roots\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

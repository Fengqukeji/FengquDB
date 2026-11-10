#include "paged/cache.hpp"
#include "paged/page.hpp"
#include "paged/generation.hpp"
#include <chrono>
#include <future>
#include <fstream>
#include <iostream>
#include <map>
#include <set>

using namespace fengqudb::paged;
static void check(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
static void page_edit_model(bool leaf) {
    struct Record {
        std::string key;
        uint64_t revision = 1, expiry = 0;
        bool present = true;
    };
    using Model = std::map<unsigned, Record>;
    auto validate = [&](const Page &page, const Model &model) {
        const auto decoded = Page::decode(page.binary);
        check(page.slots.size() == model.size() && decoded.slots.size() == model.size(),
              "page model row count");
        uint64_t maximum = 0, earliest = UINT64_MAX;
        size_t index = 0;
        for (const auto &pair : model) {
            const auto id = pair.first;
            const auto &record = pair.second;
            maximum = std::max(maximum, record.revision);
            const auto deadline = leaf ? (record.present && record.expiry ? record.expiry : UINT64_MAX)
                                       : record.expiry;
            earliest = std::min(earliest, deadline);
            for (const Page *candidate : {&page, &decoded}) {
                const auto &slot = candidate->slots[index];
                check(slot.key == record.key &&
                          candidate->binary.substr(slot.key_offset, slot.key.size()) == record.key,
                      "page edit left an invalid key view or offset");
                if (leaf)
                    check(slot.value.version == record.revision &&
                              slot.value.offset == (record.present ? 12288 + uint64_t{id} * 1024 : 0) &&
                              slot.value.size == (record.present ? id + 1 : 0) &&
                              slot.value.expires == (record.present ? record.expiry : 0) &&
                              slot.value.crc == (record.present ? id * 7 : 0),
                          "page edit changed leaf metadata");
                else
                    check(slot.maximum == record.revision && slot.earliest == record.expiry &&
                              slot.child == 12288 + uint64_t{id} * 4096,
                          "page edit changed branch metadata");
            }
            ++index;
        }
        check(page.maximum == maximum && decoded.maximum == maximum && page.earliest == earliest &&
                  decoded.earliest == earliest,
              "incremental page summary differs from the record model");
    };
    Page page(leaf);
    Model model;
    uint64_t random = 0x91e10da5c79e7b1d;
    for (unsigned step = 0; step < 512; ++step) {
        random ^= random << 13;
        random ^= random >> 7;
        random ^= random << 17;
        unsigned id = static_cast<unsigned>(random % 96);
        const bool replace_maximum = step % 11 == 0 && !model.empty();
        const bool replace_earliest = step % 13 == 0 && !model.empty();
        if (replace_maximum)
            id = std::max_element(model.begin(), model.end(), [](const auto &a, const auto &b) {
                     return a.second.revision < b.second.revision;
                 })->first;
        else if (replace_earliest)
            id = std::min_element(model.begin(), model.end(), [&](const auto &a, const auto &b) {
                     auto deadline = [&](const Record &r) {
                         return leaf ? (r.present && r.expiry ? r.expiry : UINT64_MAX) : r.expiry;
                     };
                     return deadline(a.second) < deadline(b.second);
                 })->first;
        const auto found = model.lower_bound(id);
        const bool insert = found == model.end() || found->first != id;
        const auto at = static_cast<size_t>(std::distance(model.begin(), found));
        auto snapshot = page;
        const auto before = model;
        Record record;
        const auto padding = step % 4 == 0 ? size_t{4096} : (step % 4 == 1 ? size_t{127} : size_t{0});
        record.key = "key/" + std::to_string(100000 + id) + std::string(padding, 'a');
        const bool alias = !insert && step % 5 == 0;
        if (alias)
            record.key = found->second.key;
        record.revision = replace_maximum ? 1 : 1 + random % 1000;
        record.present = random % 9 != 0 || replace_earliest;
        record.expiry = replace_earliest ? (leaf ? 0 : UINT64_MAX)
                                       : (random % 3 ? 5 + random % 100 : (leaf ? 0 : UINT64_MAX));
        if (!leaf && step % 7 == 0 && !replace_earliest)
            record.expiry = 0; // Legacy branch summaries can be unknown.
        Slot slot;
        slot.key = alias ? page.slots[at].key : std::string_view(record.key);
        if (leaf)
            slot.value = Ref{record.revision, record.present ? 12288 + uint64_t{id} * 1024 : 0,
                             record.present ? id + 1 : 0, record.present ? record.expiry : 0,
                             record.present ? id * 7 : 0};
        else {
            slot.maximum = record.revision;
            slot.earliest = record.expiry;
            slot.child = 12288 + uint64_t{id} * 4096;
        }
        page.set(at, slot, insert);
        model[id] = std::move(record);
        validate(page, model);
        if (!before.empty())
            validate(snapshot, before);
    }
    auto boundary = model.begin();
    std::advance(boundary, model.size() / 2);
    const Model left(model.begin(), boundary), right_model(boundary, model.end());
    auto old = page;
    auto right = page.split(left.size());
    validate(page, left);
    validate(right, right_model);
    validate(old, model);
}
int main() {
#ifdef __SANITIZE_THREAD__
    std::cerr << "FQDB_TSAN_STARTED\n";
#endif
    try {
        // Identities protect generation/backup ownership. A repeated or zero
        // value can attach recovery work to an unrelated database directory.
        std::set<Uuid> identities;
        for (unsigned i = 0; i < 4096; ++i) {
            const auto id = new_uuid();
            check(id != Uuid{} && identities.insert(id).second,
                  "database identities must be nonzero and distinct");
        }
        page_edit_model(true);
        page_edit_model(false);
        auto root = std::filesystem::temp_directory_path() /
                    ("fqdb-paged-io-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        check(std::filesystem::create_directory(root), "owned temporary directory");
        auto path = (root / "gen-old.fdb").u8string(), next = (root / "gen-new.fdb").u8string();
        auto manifest = (root / "CURRENT").u8string(), pending = (root / "CURRENT.next").u8string();
        {
            std::ofstream out(manifest, std::ios::binary);
            out << "gen-old.fdb";
        }
        auto old = std::make_shared<SharedFile>();
        old->open(path, true);
        old->lock();
        std::string source(4096, 'a');
        old->write(12288, source.data(), source.size());
        old->sync();
        {
            SharedFile contender;
            contender.open(path);
            bool locked = false;
            try {
                contender.lock();
            } catch (const Error &error) {
                locked = error.status == FQDB_LOCKED;
            }
            check(locked, "data-file lease");
        }
        std::vector<std::future<void>> workers;
        for (unsigned t = 0; t < 16; ++t)
            workers.push_back(std::async(std::launch::async, [&, t] {
                std::string bytes(2048, static_cast<char>(t)), read(4096, '\0');
                for (unsigned n = 0; n < 100; ++n) {
                    auto offset = 32768 + t * 2048;
                    old->write(offset, bytes.data(), bytes.size());
                    old->read(12288, read.data(), read.size());
                    check(read == source, "concurrent positional read");
                }
            }));
        for (auto &worker : workers)
            worker.get();
        for (unsigned t = 0; t < 16; ++t) {
            std::string bytes(2048, '\0');
            old->read(32768 + t * 2048, bytes.data(), bytes.size());
            check(bytes == std::string(2048, static_cast<char>(t)), "disjoint positional writes");
        }
        auto fresh = std::make_shared<SharedFile>();
        fresh->open(next, true);
        fresh->lock();
        std::string replacement(4096, 'b');
        fresh->write(12288, replacement.data(), replacement.size());
        fresh->sync();
        {
            std::ofstream out(pending, std::ios::binary);
            out << "gen-new.fdb";
        }
        auto replaced = publish_manifest(pending, manifest);
        check(replaced == FQDB_OK, "atomic generation replacement");
        {
            std::ifstream in(manifest, std::ios::binary);
            std::string current;
            in >> current;
            check(current == "gen-new.fdb", "generation manifest");
        }
        std::string read(4096, '\0');
        old->read(12288, read.data(), read.size());
        check(read == source, "old generation snapshot");
        {
            SharedFile contender;
            contender.open(next);
            bool locked = false;
            try {
                contender.lock();
            } catch (const Error &error) {
                locked = error.status == FQDB_LOCKED;
            }
            check(locked, "replacement remains leased");
        }
        fresh->unlock();
        {
            SharedFile reopened;
            reopened.open(next);
            reopened.lock();
            reopened.read(12288, read.data(), read.size());
            check(read == replacement, "new generation reopen");
        }
        Page page;
        for (unsigned i = 0; i < 40; ++i) {
            auto key = std::string("key/") + std::to_string(1000 + i);
            Slot slot;
            slot.key = key;
            slot.value = Ref{i + 1, 12288 + i * 1024, 100, 0, i};
            page.set(page.slots.size(), std::move(slot), true);
        }
        auto snapshot = page;
        auto pointer = page.binary.data();
        auto changed = page.slots[10];
        changed.value.version = 100;
        changed.value.expires = 12345;
        page.set(10, changed, false);
        check(page.binary.data() == pointer, "equal-sized page edit reallocated");
        check(snapshot.slots[10].value.version == 11 && snapshot.binary != page.binary,
              "cached page isolation");
        auto decoded = Page::decode(page.binary);
        check(decoded.slots[10].value.version == 100 && decoded.maximum == 100, "page wire update");
        auto right = page.split(19);
        check(page.slots.size() == 19 && right.slots.size() == 21, "binary subtraction split");
        check(Page::decode(page.binary).slots.back().key == "key/1018" &&
                  Page::decode(right.binary).slots.front().key == "key/1019",
              "split boundaries");
        {
            Cache<std::string> hot(1048576);
            auto keeper = std::make_shared<const std::string>(700, 'k');
            hot.put(1, keeper, keeper->capacity(), 71);
            // Rehashes must preserve the LRU's node references. Keep one hot
            // borrower through many admissions and tail evictions; use unique
            // tags to detect accidental key/node association after relinking.
            for (uint64_t i = 2; i < 20000; ++i) {
                uint64_t tag = 0;
                check(hot.get(1, &tag) == keeper && tag == 71, "hot cache key lost before admission");
                auto value = std::make_shared<const std::string>(700, static_cast<char>(i % 127));
                hot.put(i, value, value->capacity(), i + 100);
                auto returned = hot.get(i, &tag);
                check(returned == value && tag == i + 100 && hot.get(1) == keeper,
                      "cache rehash/relink lost hot key, immutable data or tag");
                if (i % 64 == 0)
                    check(hot.info().bytes <= 1048576, "hot cache exceeded its budget");
            }
            check(hot.info().evictions != 0, "hot cache did not exercise tail eviction");
            hot.configure(0);
            check(hot.info().entries == 0 && !hot.get(1) && keeper->front() == 'k',
                  "cache retirement left linked entries or invalidated a borrower");
            hot.configure(65536);
            hot.put(1, keeper, keeper->capacity(), 72);
            uint64_t tag = 0;
            check(hot.get(1, &tag) == keeper && tag == 72,
                  "cache reconfiguration retained old LRU links or metadata");
        }
        Cache<std::string> cache(65536);
        auto pinned = std::make_shared<const std::string>(1000, 'x');
        cache.put(1, pinned, 1000);
        for (uint64_t i = 2; i < 1000; ++i)
            cache.put(i, std::make_shared<const std::string>(1000, 'z'), 1000);
        check(cache.info().bytes <= 65536 && cache.info().evictions != 0 &&
                  *pinned == std::string(1000, 'x'),
              "bounded cache and pinned ownership");
        auto large = std::make_shared<const std::string>(32768, 'b');
        cache.put(1001, large, large->capacity());
        check(cache.get(1001) == large && cache.info().bytes <= 65536, "large hot value admission");
        workers.clear();
        for (unsigned t = 0; t < 8; ++t)
            workers.push_back(std::async(std::launch::async, [&, t] {
                for (unsigned n = 0; n < 150; ++n) {
                    auto size = n % 13 == 0 ? 30000 : 700;
                    auto value = std::make_shared<const std::string>(size, static_cast<char>(t));
                    auto key = uint64_t{2000} + t * 200 + n;
                    cache.put(key, value, value->capacity());
                    (void)cache.get(key);
                    check(cache.info().bytes <= 65536, "concurrent cache exceeded global budget");
                }
            }));
        for (auto &worker : workers)
            worker.get();
        check(*large == std::string(32768, 'b'), "large pinned value invalidated by eviction");
        cache.configure(0);
        cache.put(9000, large, large->capacity());
        check(cache.info().bytes == 0 && !cache.get(9000), "retired cache admitted values");
        cache.configure(65536);
        cache.put(9000, large, large->capacity());
        check(cache.get(9000) == large, "cache reconfiguration lost immutable borrower");
        workers.clear();
        workers.push_back(std::async(std::launch::async, [&] {
            for (unsigned i = 0; i < 150; ++i)
                cache.configure(i % 2 ? 65536 : 0);
        }));
        for (unsigned t = 0; t < 4; ++t)
            workers.push_back(std::async(std::launch::async, [&, t] {
                for (unsigned n = 0; n < 150; ++n) {
                    auto value = std::make_shared<const std::string>(n % 5 ? 700 : 30000, 'q');
                    auto key = uint64_t{10000} + t * 200 + n;
                    cache.put(key, value, value->capacity());
                    (void)cache.get(key);
                    check(cache.info().bytes <= 65536,
                          "cache reconfiguration exceeded global budget");
                }
            }));
        for (auto &worker : workers)
            worker.get();
        cache.configure(0);
        check(cache.info().bytes == 0 && *large == std::string(32768, 'b'),
              "retired borrower after concurrent reconfiguration");
        fresh.reset();
        old.reset();
        check(std::filesystem::remove(std::filesystem::u8path(path)), "remove owned database");
        check(std::filesystem::remove(std::filesystem::u8path(next)), "remove owned generation");
        check(std::filesystem::remove(std::filesystem::u8path(manifest)), "remove owned manifest");
        check(std::filesystem::remove(root), "remove empty owned directory");
        std::cout << "PASS paged I/O, file leases, immutable generation replacement, bounded cache "
                     "and binary subtraction\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

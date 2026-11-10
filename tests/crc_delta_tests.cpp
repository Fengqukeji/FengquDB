#include "store_test_support.hpp"
#include "paged/operations.hpp"
#include <random>
namespace s = fengqudb::storage;
static uint32_t reference_crc(const void *data, size_t size, uint32_t initial = 0) {
    auto bytes = static_cast<const unsigned char *>(data);
    auto crc = ~initial;
    while (size--) {
        crc ^= *bytes++;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0u);
    }
    return ~crc;
}
static void crc_chains() {
    check(s::crc32(nullptr, 0, 123) == 123, "empty seeded CRC");
    check(s::crc32("123456789", 9) == 0xcbf43926u, "IEEE CRC known vector");
    std::mt19937_64 rng(20261110);
    std::string bytes(131200, 'x');
    for (auto &v : bytes)
        v = char(rng());
    for (unsigned n = 0; n < 12000; ++n) {
        size_t size = n < 4096 ? n : rng() % 131072;
        auto at = size_t(rng() % 64);
        auto initial = uint32_t(rng());
        auto crc = s::crc32(bytes.data() + at, size, initial);
        check(crc == s::crc32_scalar(bytes.data() + at, size, initial), "independent CRC chains");
        if (n % 31 == 0)
            check(crc == reference_crc(bytes.data() + at, size, initial), "bitwise CRC mismatch");
        auto split = size_t(rng() % (size + 1));
        check(s::crc32_combine(s::crc32(bytes.data() + at, split, initial),
                               s::crc32(bytes.data() + at + split, size - split),
                               size - split) == crc,
              "cached CRC combination");
        auto length = rng();
        auto a = uint32_t(rng()), b = uint32_t(rng());
        check(s::crc32_combine(a, b, length) == s::crc32_combine_raw(a, b, length),
              "64-bit CRC shift cache");
    }
}
static void matches(const std::shared_ptr<p::StagedData> &bytes) {
    auto cached = bytes->cached_crc.load();
    check(!cached || uint32_t(cached) == s::crc32(bytes->data(), bytes->size()),
          "cached CRC mismatch");
    check(p::value_crc(bytes) == s::crc32(bytes->data(), bytes->size()), "final CRC mismatch");
}
static void random_edits() {
    std::mt19937_64 rng(20261109);
    auto budget = std::make_shared<p::Budget>(64 << 20);
    for (unsigned n = 0; n < 12000; ++n) {
        p::Data original(size_t(rng() % 8192), 'a');
        for (auto &c : original)
            c = char(rng());
        if (n % 3)
            original.cached_crc.store((UINT64_C(1) << 32) |
                                      s::crc32(original.data(), original.size()));
        auto bytes = p::staged_data(original, budget, budget);
        check(bytes->cached_crc.load() == original.cached_crc.load(), "clone lost CRC cache");
        std::string expected(original);
        for (unsigned step = 0; step < 5; ++step) {
            auto at = size_t(rng() % (bytes->size() + 1));
            auto remove = size_t(rng() % (bytes->size() - at + 1));
            std::string insert(size_t(rng() % 128), 'b');
            for (auto &c : insert)
                c = char(rng());
            if (step % 2)
                remove = std::min(insert.size(), bytes->size() - at);
            p::patch_buffer(*bytes, at, remove, insert);
            expected.replace(at, remove, insert);
            check(*bytes == expected, "subtractive edit mismatch");
            matches(bytes);
            auto value = uint32_t(rng());
            auto length = rng();
            check(s::crc32_shift(s::crc32_shift(value, length), length, true) == value,
                  "CRC inverse mismatch");
        }
    }
    check(budget->used() == 0, "private budget retained");
    p::Data zero("\x9d\x0a\xd9\x6d", 4);
    zero.cached_crc.store(UINT64_C(1) << 32);
    check(s::crc32(zero.data(), zero.size()) == 0, "zero CRC vector");
    auto bytes = p::staged_data(zero, budget, {});
    check(bytes->cached_crc.load() == (UINT64_C(1) << 32), "known zero CRC lost");
    p::patch_buffer(*bytes, 4, 0, std::string(8192, 'z'));
    matches(bytes);
    check(bytes->cached_crc.load(), "append did not extend cached CRC");
    p::patch_buffer(*bytes, 0, bytes->size(), {});
    check(bytes->cached_crc.load() == (UINT64_C(1) << 32), "empty CRC marker");
    p::Data large(2 << 20, 'x');
    bytes = p::staged_data(large, budget, {});
    bytes->cached_crc.store((UINT64_C(1) << 32) | s::crc32(bytes->data(), bytes->size()));
    p::patch_buffer(*bytes, 1, bytes->size() - 2, "Q");
    check(*bytes == "xQx", "large crop mismatch");
    matches(bytes);
}
static void durable_edits(bool segmented) {
    Fixture fixture;
    auto path = fixture.name("crc");
    auto o = p::defaults();
    if (segmented)
        o.flags |= FQDB_STORE_SEGMENTED;
    auto core = std::make_shared<p::Core>(path, o);
    core->open();
    core->configure_write(
        {sizeof(fqdb_store_write_options), 1, FQDB_WRITE_PERFORMANCE, 100, 1048576});
    std::string expected(65536, 'x');
    {
        p::Transaction tx(core, 0);
        tx.put("k", expected, 0);
        core->commit(tx);
    }
    for (unsigned n = 0; n < 40; ++n) {
        auto old = expected;
        p::Transaction tx(core, 0);
        auto alias = tx.get("k");
        auto save = tx.savepoint();
        auto at = n % 2 ? size_t(13) : expected.size() - 17;
        std::string insert(n % 3 == 0 ? 11 : 8, char('a' + n % 26));
        tx.patch("k", at, 8, insert);
        auto private_value = tx.get("k");
        tx.patch("k", 1, 4, "private");
        tx.rollback_to(save);
        tx.patch("k", at, 8, insert);
        expected.replace(at, 8, insert);
        core->commit(tx, nullptr, n % 7 == 0);
        check(*alias == old, "snapshot alias changed");
        check(private_value->size() == expected.size(), "saved private alias changed");
        auto view = core->current();
        auto ref = view->find("k");
        auto value = view->generation->value(*ref, view->limit);
        check(*value == expected && p::value_crc(value) == ref->crc, "committed delta checksum");
    }
    core->sync();
    core.reset();
    core = std::make_shared<p::Core>(path, o);
    core->open();
    auto view = core->current();
    auto ref = view->find("k");
    check(*view->generation->value(*ref, view->limit) == expected, "durable delta reopen");
}
template <class F> static void corrupt(F &&operation) {
    bool rejected = false;
    try {
        operation();
    } catch (const p::Error &e) {
        rejected = e.status == FQDB_CORRUPT;
    }
    check(rejected, "corrupt cold value accepted");
}
static void cold_value_checks(bool segmented) {
    Fixture fixture;
    auto directory = fixture.name("single-check");
    check(std::filesystem::create_directory(std::filesystem::u8path(directory)),
          "fixture directory");
    auto generation = std::make_shared<p::Generation>(0);
    auto id = p::new_uuid();
    generation->create(p::join(directory, p::generation_name(id, 1)), id, 1,
                       segmented ? p::new_segment_geometry() : 0);
    uint64_t version = 1;
    for (size_t size :
         {size_t(0), size_t(1), size_t(128), size_t(4093), size_t(65533), size_t(1048573)}) {
        std::string expected(size, 'q');
        if (size)
            expected.back() = 'z';
        p::WalBatch batch(version++, p::unix_ms());
        batch.add(p::OpKind::put, "k", std::make_shared<const p::Data>(expected));
        auto ref = batch.append(*generation)[0];
        generation->file->sync();
        s::crc32_watch_size = size;
        s::crc32_watch_calls = 0;
        auto value = generation->value(ref, generation->tail, false, false);
        check(*value == expected && s::crc32_watch_calls == 1,
              "cold raw value must have one logical CRC");
        auto invalid = ref;
        invalid.crc ^= 1;
        s::crc32_watch_calls = 0;
        corrupt([&] { generation->value(invalid, generation->tail, false, false); });
        check(s::crc32_watch_calls == 1, "invalid metadata bypassed final CRC");
        generation->values.configure(4 << 20);
        s::crc32_watch_calls = 0;
        auto cached = generation->value(ref, generation->tail);
        auto again = generation->value(ref, generation->tail);
        check(*cached == expected && again.get() == cached.get() && s::crc32_watch_calls == 1,
              "cached read rescanned immutable bytes");
        generation->values.configure(0);
        if (size) {
            auto at = size / 2;
            char changed = char(expected[at] ^ 1);
            generation->file->write(ref.offset + at, &changed, 1);
            corrupt([&] { generation->value(ref, generation->tail, false, false); });
            generation->file->write(ref.offset + at, expected.data() + at, 1);
        }
        s::crc32_watch_size = size_t(-1);
    }
    // A cold patch still validates its final reconstructed value once.
    std::string expected(4093, 'a');
    p::WalBatch raw(version++, 0);
    raw.add(p::OpKind::put, "patch", std::make_shared<const p::Data>(expected));
    auto ref = raw.append(*generation)[0];
    expected.replace(15, 8, "12345678");
    p::WalBatch patch(version++, 0);
    patch.add(p::OpKind::patch, "patch", std::make_shared<const p::Data>("12345678"), ref, 0, 15, 8,
              reference_crc(expected.data(), expected.size()));
    ref = patch.append(*generation)[0];
    s::crc32_watch_size = expected.size();
    s::crc32_watch_calls = 0;
    check(*generation->value(ref, generation->tail, false, false) == expected &&
              s::crc32_watch_calls == 1,
          "cold patch final CRC changed");
    s::crc32_watch_size = size_t(-1);
}
int main() {
    try {
        cold_value_checks(false);
        cold_value_checks(true);
        crc_chains();
        random_edits();
        durable_edits(false);
        durable_edits(true);
        std::cout << "PASS delta CRC, cropping, aliases and durable reopen\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

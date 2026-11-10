#pragma once
#include "select_parse.hpp"

namespace fengqudb::paged::select {
class Memory {
    Charge charge_;
    uint64_t maximum_, used_ = 0, base_, charged_base_ = 0;

  public:
    uint64_t peak;
    Memory(Transaction &tx, uint64_t maximum, uint64_t base)
        : charge_(tx.budget, tx.base->core->transaction_budget, 0), maximum_(maximum), base_(base),
          peak(base) {
        require(base <= maximum, FQDB_LIMIT);
    }
    Memory(Charge allocation, uint64_t maximum, uint64_t base)
        : charge_(std::move(allocation)), maximum_(maximum), base_(base), charged_base_(base),
          peak(base) {
        require(base <= maximum, FQDB_LIMIT);
    }
    uint64_t retained() const noexcept {
        return base_ + used_;
    }
    void take(size_t n) {
        require(n <= maximum_ - base_ - used_, FQDB_LIMIT);
        charge_.resize(charged_base_ + used_ + n);
        used_ += n;
        peak = std::max(peak, used_ + base_);
    }
    void release(size_t n) noexcept {
        used_ -= n;
        charge_.resize(charged_base_ + used_);
    }
};
struct Hold {
    Memory *memory;
    size_t bytes = 0;
    explicit Hold(Memory &m) : memory(&m) {}
    Hold(const Hold &) = delete;
    Hold &operator=(const Hold &) = delete;
    Hold(Hold &&o) noexcept : memory(o.memory), bytes(std::exchange(o.bytes, 0)) {}
    ~Hold() {
        if (bytes)
            memory->release(bytes);
    }
    void resize(size_t n) {
        if (n > bytes)
            memory->take(n - bytes);
        else
            memory->release(bytes - n);
        bytes = n;
    }
};
template <class T> struct Alloc {
    using value_type = T;
    using propagate_on_container_move_assignment = std::true_type;
    Memory *memory = nullptr;
    Alloc() = default;
    explicit Alloc(Memory &m) : memory(&m) {}
    template <class U> Alloc(const Alloc<U> &o) : memory(o.memory) {}
    T *allocate(size_t n) {
        require(memory && n <= SIZE_MAX / sizeof(T), FQDB_LIMIT);
        size_t bytes = n * sizeof(T);
        memory->take(bytes);
        try {
            return static_cast<T *>(::operator new(bytes));
        } catch (...) {
            memory->release(bytes);
            throw;
        }
    }
    void deallocate(T *p, size_t n) noexcept {
        ::operator delete(p);
        memory->release(n * sizeof(T));
    }
    template <class U> bool operator==(const Alloc<U> &o) const noexcept {
        return memory == o.memory;
    }
    template <class U> bool operator!=(const Alloc<U> &o) const noexcept {
        return memory != o.memory;
    }
};
template <class T> using Vec = std::vector<T, Alloc<T>>;
using Text = std::basic_string<char, std::char_traits<char>, Alloc<char>>;
inline std::string_view view(const Text &s) {
    return {s.data(), s.size()};
}
inline Text text(Memory &m, std::string_view s) {
    return Text(s.data(), s.size(), Alloc<char>(m));
}
struct Hash {
    size_t operator()(const Text &s) const noexcept {
        return std::hash<std::string_view>{}(view(s));
    }
};
template <class V>
using Map = std::unordered_map<Text, V, Hash, std::equal_to<Text>, Alloc<std::pair<const Text, V>>>;
using Set = std::unordered_set<Text, Hash, std::equal_to<Text>, Alloc<Text>>;
struct Owned {
    Atom scalar;
    Text bytes;
    explicit Owned(Memory &m) : bytes(Alloc<char>(m)) {}
    Owned(Memory &m, const Atom &a)
        : scalar(a), bytes(a.kind == Kind::Bytes ? text(m, a.text) : Text(Alloc<char>(m))) {
        scalar.text = {};
    }
    Atom ref() const {
        auto a = scalar;
        if (a.kind == Kind::Bytes)
            a.text = view(bytes);
        return a;
    }
};
struct Work {
    Transaction &tx;
    const Spec &spec;
    QueryStats stats;
    std::chrono::steady_clock::time_point deadline;
    uint64_t checked = 0;
    Work(Transaction &t, const Spec &s,
         std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now())
        : tx(t), spec(s), deadline(start + std::chrono::milliseconds(s.timeout_ms)) {}
    void tick(uint64_t n = 1) {
        require(n <= spec.max_work - stats.work, FQDB_LIMIT);
        stats.work += n;
        if (stats.work - checked >= 512) {
            checked = stats.work;
            tx.ready();
            require(std::chrono::steady_clock::now() < deadline, FQDB_TIMEOUT);
        }
    }
    void candidate() {
        require(stats.candidates < spec.max_candidates, FQDB_LIMIT);
        ++stats.candidates;
        tick();
    }
    void check() {
        tx.ready();
        require(std::chrono::steady_clock::now() < deadline, FQDB_TIMEOUT);
    }
};
} // namespace fengqudb::paged::select

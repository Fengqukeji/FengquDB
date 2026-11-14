#pragma once
// Original persistent HNSW. INT8 is only a navigation representation; results
// are rescored from the authoritative FP32 projection in the supplied view.
#include "storage.hpp"
#include <queue>
#include <unordered_set>

namespace fengqudb::index::impl {
struct VectorContract {
    uint64_t dimension = 0, count = 0, generation = 0, building = 0;
    bool counted = false, enabled = false;
};
inline VectorContract vector_contract(const Reader &r, const std::string &base,
                                      std::string_view space) {
    auto b = r.get(base + "V" + component(space));
    VectorContract c;
    if (!b.found)
        return c;
    fields(b.value, [&](w::Reader &a, w::Field f) {
        if (f.number == 1)
            c.dimension = number(a, f);
        else if (f.number == 2) {
            c.count = number(a, f);
            c.counted = true;
        } else if (f.number == 3)
            c.generation = number(a, f);
        else if (f.number == 4)
            c.building = number(a, f);
        else if (f.number == 5) {
            auto n = number(a, f);
            require(n <= 1, FQDB_CORRUPT);
            c.enabled = n != 0;
        } else
            return false;
        return true;
    });
    require(c.dimension > 0 && c.dimension <= 4096 &&
                (!c.building || c.building == c.generation + 1),
            FQDB_CORRUPT);
    return c;
}
inline void save_contract(const Reader &r, const std::string &base, std::string_view space,
                          const VectorContract &c) {
    w::Writer w;
    w.uint_field(1, c.dimension);
    if (c.counted)
        w.uint_field(2, c.count);
    w.uint_field(3, c.generation);
    w.uint_field(4, c.building);
    w.uint_field(5, c.enabled);
    r.put(base + "V" + component(space), w.data());
}
inline void vector_count(const Reader &r, const std::string &base, std::string_view space,
                         int delta) {
    auto c = vector_contract(r, base, space);
    require(c.dimension, FQDB_CORRUPT);
    if (!c.counted)
        return; // Unknown acceleration coverage uses exact search until repair.
    if (delta > 0) {
        require(c.count < UINT64_MAX, FQDB_LIMIT);
        ++c.count;
    } else {
        require(c.count > 0, FQDB_CORRUPT);
        --c.count;
    }
    save_contract(r, base, space, c);
}
struct AnnNode {
    std::string id, quantized;
    uint64_t generation = 0;
    bool alive = true;
    unsigned level = 0;
    double inverse_norm = 1;
    std::vector<std::vector<std::string>> links;
};
inline unsigned ann_level(std::string_view id) {
    uint64_t h = UINT64_C(1469598103934665603);
    for (unsigned char c : id)
        h = (h ^ c) * UINT64_C(1099511628211);
    h ^= h >> 30;
    h *= UINT64_C(0xbf58476d1ce4e5b9);
    h ^= h >> 27;
    h *= UINT64_C(0x94d049bb133111eb);
    h ^= h >> 31;
    unsigned level = 0;
    while (level < 8 && h % 12 == 0) {
        ++level;
        h /= 12;
    }
    return level;
}
inline AnnNode ann_node(const Document &d) {
    AnnNode n;
    n.id = d.id;
    n.generation = d.generation;
    n.level = ann_level(d.id);
    n.links.resize(n.level + 1);
    double maximum = 0, sum = 0;
    for (float f : d.vector)
        maximum = std::max(maximum, std::abs(double(f)));
    require(maximum > 0);
    for (float f : d.vector) {
        int q = static_cast<int>(std::lround(double(f) / maximum * 127));
        n.quantized.push_back(static_cast<char>(static_cast<unsigned char>(q & 255)));
        sum += q * q;
    }
    n.inverse_norm = 1 / std::sqrt(sum);
    return n;
}
inline int ann_byte(char c) {
    unsigned x = static_cast<unsigned char>(c);
    return x < 128 ? int(x) : int(x) - 256;
}
inline std::vector<float> ann_vector(const AnnNode &n) {
    std::vector<float> v;
    v.reserve(n.quantized.size());
    for (char c : n.quantized)
        v.push_back(static_cast<float>(ann_byte(c) * n.inverse_norm));
    return v;
}
inline double ann_similarity(const std::vector<float> &q, const AnnNode &n) {
    require(q.size() == n.quantized.size(), FQDB_CORRUPT);
    double sum = 0;
    for (size_t i = 0; i < q.size(); ++i)
        sum += q[i] * ann_byte(n.quantized[i]);
    return std::clamp(sum * n.inverse_norm, -1., 1.);
}
inline std::string encode(const AnnNode &n) {
    w::Writer w;
    w.bytes_field(1, n.id);
    w.bytes_field(2, n.quantized);
    w.double_field(3, n.inverse_norm);
    w.uint_field(4, n.level);
    w.uint_field(5, n.alive);
    w.uint_field(6, n.generation);
    for (unsigned level = 0; level < n.links.size(); ++level) {
        w::Writer l;
        l.uint_field(1, level);
        for (const auto &id : n.links[level])
            l.bytes_field(2, id);
        w.bytes_field(7, l.data());
    }
    return w.take();
}
inline AnnNode ann_decode(std::string_view data) {
    AnnNode n;
    fields(data, [&](w::Reader &r, w::Field f) {
        switch (f.number) {
        case 1:
            n.id = bytes(r, f);
            break;
        case 2:
            n.quantized = bytes(r, f);
            break;
        case 3:
            n.inverse_norm = real(r, f);
            break;
        case 4: {
            auto v = number(r, f);
            require(v <= 8, FQDB_CORRUPT);
            n.level = unsigned(v);
            break;
        }
        case 5: {
            auto v = number(r, f);
            require(v <= 1, FQDB_CORRUPT);
            n.alive = v != 0;
            break;
        }
        case 6:
            n.generation = number(r, f);
            break;
        case 7: {
            unsigned level = 9;
            std::vector<std::string> links;
            fields(bytes(r, f), [&](w::Reader &a, w::Field q) {
                if (q.number == 1) {
                    auto v = number(a, q);
                    require(v <= 8, FQDB_CORRUPT);
                    level = unsigned(v);
                } else if (q.number == 2) {
                    auto id = bytes(a, q);
                    require(!id.empty() && id.size() <= 1024 && links.size() < 24, FQDB_CORRUPT);
                    links.emplace_back(id);
                } else
                    return false;
                return true;
            });
            require(level <= 8, FQDB_CORRUPT);
            if (n.links.size() <= level)
                n.links.resize(level + 1);
            require(n.links[level].empty(), FQDB_CORRUPT);
            n.links[level] = std::move(links);
            break;
        }
        default:
            return false;
        }
        return true;
    });
    require(!n.id.empty() && n.id.size() <= 1024 && !n.quantized.empty() &&
                n.quantized.size() <= 4096 && n.inverse_norm > 0 && n.inverse_norm <= 1 &&
                n.links.size() == n.level + 1,
            FQDB_CORRUPT);
    return n;
}
struct AnnHead {
    std::string entry;
    unsigned level = 0;
    uint64_t active = 0;
};
struct AnnItem {
    std::string id;
    double score;
};
inline bool ann_better(const AnnItem &a, const AnnItem &b) {
    return a.score != b.score ? a.score > b.score : a.id < b.id;
}
class AnnGraph {
    const Reader &r_;
    std::string prefix_;

  public:
    AnnGraph(const Reader &r, const std::string &base, std::string_view space,
             std::optional<uint64_t> generation = {})
        : r_(r), prefix_(base + "H" + component(space) + "G") {
        auto value = generation ? *generation : vector_contract(r, base, space).generation;
        for (int shift = 56; shift >= 0; shift -= 8)
            prefix_.push_back(static_cast<char>((value >> shift) & 255));
    }
    AnnHead head() const {
        auto b = r_.get(prefix_ + "M");
        AnnHead h;
        if (!b.found)
            return h;
        fields(b.value, [&](w::Reader &r, w::Field f) {
            if (f.number == 1)
                h.entry = bytes(r, f);
            else if (f.number == 2) {
                auto n = number(r, f);
                require(n <= 8, FQDB_CORRUPT);
                h.level = unsigned(n);
            } else if (f.number == 3)
                h.active = number(r, f);
            else
                return false;
            return true;
        });
        require(h.entry.size() <= 1024, FQDB_CORRUPT);
        return h;
    }
    void save(const AnnHead &h) const {
        w::Writer w;
        w.bytes_field(1, h.entry);
        w.uint_field(2, h.level);
        w.uint_field(3, h.active);
        r_.put(prefix_ + "M", w.data());
    }
    AnnNode get(std::string_view id) const {
        auto b = r_.get(prefix_ + "N" + component(id));
        require(b.found, FQDB_CORRUPT);
        auto n = ann_decode(b.value);
        require(n.id == id, FQDB_CORRUPT);
        return n;
    }
    bool exists(std::string_view id) const {
        return r_.get(prefix_ + "N" + component(id)).found;
    }
    void put(const AnnNode &n) const {
        r_.put(prefix_ + "N" + component(n.id), encode(n));
    }
    AnnItem greedy(const std::vector<float> &q, AnnItem entry, unsigned level, Work &work,
                   uint64_t &visited, uint64_t budget) const {
        bool changed = true;
        while (changed) {
            changed = false;
            work.check();
            if (visited >= budget)
                throw Stop{FQIDX_SCAN_LIMIT};
            auto node = get(entry.id);
            ++visited;
            ScopedCharge charge(work, node.quantized.size() + node.id.size() * 25 + 4096);
            if (level >= node.links.size())
                continue;
            for (const auto &id : node.links[level]) {
                if (visited >= budget)
                    throw Stop{FQIDX_SCAN_LIMIT};
                auto neighbor = get(id);
                ++visited;
                AnnItem item{id, ann_similarity(q, neighbor)};
                if (ann_better(item, entry)) {
                    entry = std::move(item);
                    changed = true;
                }
            }
        }
        return entry;
    }
    std::vector<AnnItem> layer(const std::vector<float> &q, const AnnItem &entry, unsigned level,
                               unsigned ef, Work &work, uint64_t &visited, uint64_t budget) const {
        struct Pending {
            AnnItem item;
            std::vector<std::string> links;
        };
        auto order = [](const Pending &a, const Pending &b) { return ann_better(b.item, a.item); };
        std::vector<Pending> pending;
        std::priority_queue<AnnItem, std::vector<AnnItem>, decltype(&ann_better)> best(&ann_better);
        std::unordered_set<std::string> seen;
        struct Memory {
            Work &w;
            uint64_t used = 0;
            ~Memory() {
                w.release(used);
            }
            void take(uint64_t n) {
                w.take(n);
                used += n;
            }
        } memory{work};
        auto enqueue = [&](AnnItem item, AnnNode node) {
            std::vector<std::string> links;
            if (level < node.links.size())
                links = std::move(node.links[level]);
            uint64_t charge = item.id.size() + 128;
            for (const auto &id : links)
                charge += id.size() + 48;
            memory.take(charge);
            pending.push_back({item, std::move(links)});
            std::push_heap(pending.begin(), pending.end(), order);
            best.push(std::move(item));
            if (best.size() > ef)
                best.pop();
        };
        if (visited >= budget)
            throw Stop{FQIDX_SCAN_LIMIT};
        auto first = get(entry.id);
        ++visited;
        seen.insert(entry.id);
        enqueue(entry, std::move(first));
        while (!pending.empty()) {
            work.check();
            std::pop_heap(pending.begin(), pending.end(), order);
            auto current = std::move(pending.back());
            pending.pop_back();
            if (best.size() >= ef && ann_better(best.top(), current.item))
                break;
            for (const auto &id : current.links) {
                if (seen.contains(id))
                    continue;
                if (visited >= budget)
                    throw Stop{FQIDX_SCAN_LIMIT};
                auto node = get(id);
                ++visited;
                AnnItem item{id, ann_similarity(q, node)};
                memory.take(id.size() + 96);
                seen.insert(id);
                if (best.size() < ef || ann_better(item, best.top()))
                    enqueue(std::move(item), std::move(node));
            }
        }
        std::vector<AnnItem> out;
        out.reserve(best.size());
        while (!best.empty()) {
            out.push_back(best.top());
            best.pop();
        }
        std::sort(out.begin(), out.end(), ann_better);
        return out;
    }
    std::vector<AnnItem> search(const std::vector<float> &q, unsigned ef, Work &work,
                                uint64_t &visited, uint64_t budget) const {
        auto h = head();
        if (h.entry.empty() || !h.active)
            return {};
        require(visited < budget, FQDB_LIMIT);
        auto n = get(h.entry);
        ++visited;
        AnnItem at{h.entry, ann_similarity(q, n)};
        for (unsigned l = h.level; l > 0; --l)
            at = greedy(q, std::move(at), l, work, visited, budget);
        return layer(q, at, 0, ef, work, visited, budget);
    }
    void erase(std::string_view id) const {
        if (!exists(id))
            return;
        auto n = get(id);
        if (!n.alive)
            return;
        n.alive = false;
        put(n);
        auto h = head();
        require(h.active > 0, FQDB_CORRUPT);
        --h.active;
        save(h);
        // Dead nodes remain traversable so deletion cannot disconnect live nodes.
    }
    std::vector<std::string> select(const std::vector<AnnItem> &ranked, unsigned maximum,
                                    std::string_view excluded) const {
        std::vector<std::string> chosen;
        std::vector<AnnNode> representatives;
        for (const auto &item : ranked) {
            if (item.id == excluded)
                continue;
            auto node = get(item.id);
            auto query = ann_vector(node);
            bool diverse = true;
            for (const auto &representative : representatives)
                if (ann_similarity(query, representative) >= item.score - 1e-12) {
                    diverse = false;
                    break;
                }
            if (diverse) {
                chosen.push_back(item.id);
                representatives.push_back(std::move(node));
            }
            if (chosen.size() == maximum)
                break;
        }
        // Keep the degree useful for tiny/duplicate clusters after applying the
        // HNSW diversity rule; exact fallback handles indistinguishable INT8 ties.
        for (const auto &item : ranked) {
            if (chosen.size() == maximum)
                break;
            if (item.id != excluded &&
                std::find(chosen.begin(), chosen.end(), item.id) == chosen.end())
                chosen.push_back(item.id);
        }
        return chosen;
    }
    void upsert(const Document &d, Work *external_work = nullptr) const {
        auto h = head();
        auto node = ann_node(d);
        if (exists(d.id)) {
            auto old = get(d.id);
            if (old.quantized == node.quantized) {
                old.generation = d.generation;
                if (!old.alive) {
                    require(h.active < UINT64_MAX, FQDB_LIMIT);
                    ++h.active;
                }
                old.alive = true;
                put(old);
                save(h);
                return;
            }
            // Keep old navigation edges as a bounded starting point, then
            // reconnect at the new location. Reverse edges are repaired below.
            node.links = old.links;
            if (!old.alive)
                ++h.active;
        } else {
            require(h.active < UINT64_MAX, FQDB_LIMIT);
            ++h.active;
        }
        if (h.entry.empty()) {
            h.entry = d.id;
            h.level = node.level;
            put(node);
            save(h);
            return;
        }
        put(node);
        Work local_work;
        local_work.memory = 32ull << 20;
        Work &work = external_work ? *external_work : local_work;
        uint64_t visited = 0;
        auto q = ann_vector(node);
        auto first = get(h.entry);
        AnnItem entry{h.entry, ann_similarity(q, first)};
        for (unsigned l = h.level; l > node.level; --l)
            entry = greedy(q, std::move(entry), l, work, visited, 20000);
        for (int level = static_cast<int>(std::min(h.level, node.level)); level >= 0; --level) {
            auto candidates = layer(q, entry, unsigned(level), 96, work, visited, 20000);
            unsigned maximum = level == 0 ? 24 : 12;
            auto &links = node.links[static_cast<size_t>(level)];
            links = select(candidates, maximum, node.id);
            put(node);
            for (const auto &id : links) {
                auto other = get(id);
                if (other.links.size() <= static_cast<size_t>(level))
                    continue;
                auto &back = other.links[static_cast<size_t>(level)];
                if (std::find(back.begin(), back.end(), node.id) == back.end())
                    back.push_back(node.id);
                if (back.size() > maximum) {
                    auto query = ann_vector(other);
                    std::vector<AnnItem> ranked;
                    for (const auto &neighbor : back)
                        ranked.push_back({neighbor, ann_similarity(query, get(neighbor))});
                    std::sort(ranked.begin(), ranked.end(), ann_better);
                    back = select(ranked, maximum, other.id);
                }
                put(other);
            }
            if (!candidates.empty())
                entry = candidates.front();
        }
        if (node.level > h.level) {
            h.entry = node.id;
            h.level = node.level;
        }
        save(h);
    }
};
} // namespace fengqudb::index::impl

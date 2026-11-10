#pragma once
#include "select_memory.hpp"
#include <array>

namespace fengqudb::paged::select {
inline unsigned fold(unsigned c) {
    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}
class Pattern {
    struct Node {
        int kind = 0, a = -1, b = -1, minimum = 0, maximum = 0;
        std::array<uint64_t, 4> bits{};
    };
    struct State {
        int kind = 0, a = -1, b = -1;
        std::array<uint64_t, 4> bits{};
    };
    struct Fragment {
        int start;
        std::vector<std::pair<int, bool>> ends;
    };
    Vec<Node> nodes_;
    Vec<State> states_;
    std::string_view input_;
    size_t at_ = 0;
    bool insensitive_ = false;
    Work *compiler_ = nullptr;
    int root_ = -1, start_ = -1;
    static void set(std::array<uint64_t, 4> &bits, unsigned c) {
        bits[c / 64] |= UINT64_C(1) << (c % 64);
    }
    static bool has(const std::array<uint64_t, 4> &bits, unsigned c) {
        return (bits[c / 64] >> (c % 64)) & 1;
    }
    int node(Node n) {
        if (compiler_)
            compiler_->tick();
        require(nodes_.size() < 2048, FQDB_LIMIT);
        nodes_.push_back(n);
        return int(nodes_.size() - 1);
    }
    int pair(int kind, int a, int b) {
        Node n;
        n.kind = kind;
        n.a = a;
        n.b = b;
        return node(n);
    }
    int balanced(std::vector<int> list, int kind) {
        if (list.empty())
            return node({});
        while (list.size() > 1) {
            size_t out = 0;
            for (size_t i = 0; i < list.size(); i += 2)
                list[out++] = i + 1 < list.size() ? pair(kind, list[i], list[i + 1]) : list[i];
            list.resize(out);
        }
        return list[0];
    }
    unsigned literal() {
        require(at_ < input_.size(), FQDB_INVALID_ARGUMENT);
        return static_cast<unsigned char>(input_[at_++]);
    }
    std::array<uint64_t, 4> escaped() {
        std::array<uint64_t, 4> b{};
        auto c = literal();
        bool invert = c == 'D' || c == 'W' || c == 'S';
        auto k = fold(c);
        if (k == 'd') {
            for (unsigned i = '0'; i <= '9'; ++i)
                set(b, i);
        } else if (k == 'w') {
            for (unsigned i = '0'; i <= '9'; ++i)
                set(b, i);
            for (unsigned i = 'a'; i <= 'z'; ++i) {
                set(b, i);
                set(b, i - 32);
            }
            set(b, '_');
        } else if (k == 's') {
            for (unsigned i : {9u, 10u, 11u, 12u, 13u, 32u})
                set(b, i);
        } else if (c == 'n' || c == 'r' || c == 't')
            set(b, c == 'n' ? 10 : c == 'r' ? 13 : 9);
        else if (c == 'x') {
            unsigned v = 0;
            for (int i = 0; i < 2; ++i) {
                auto h = literal();
                unsigned d = h >= '0' && h <= '9'               ? h - '0'
                             : fold(h) >= 'a' && fold(h) <= 'f' ? fold(h) - 'a' + 10
                                                                : 256;
                require(d < 16, FQDB_INVALID_ARGUMENT);
                v = v * 16 + d;
            }
            set(b, v);
        } else {
            require(!(c >= '0' && c <= '9') && !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')),
                    FQDB_UNSUPPORTED);
            set(b, c);
        }
        if (invert)
            for (auto &word : b)
                word = ~word;
        return b;
    }
    int atom(unsigned depth) {
        require(depth <= 64, FQDB_LIMIT);
        auto c = literal();
        Node n;
        n.kind = 1;
        if (c == '(') {
            if (at_ < input_.size() && input_[at_] == '?') {
                require(at_ + 1 < input_.size() && input_[at_ + 1] == ':', FQDB_UNSUPPORTED);
                at_ += 2;
            }
            int result = alternation(depth + 1);
            require(at_ < input_.size() && input_[at_++] == ')', FQDB_INVALID_ARGUMENT);
            return result;
        }
        if (c == '^' || c == '$') {
            n.kind = c == '^' ? 5 : 6;
            return node(n);
        }
        if (c == '.') {
            for (auto &w : n.bits)
                w = UINT64_MAX;
            n.bits[10 / 64] &= ~(UINT64_C(1) << (10 % 64));
        } else if (c == '\\')
            n.bits = escaped();
        else if (c == '[') {
            bool invert = at_ < input_.size() && input_[at_] == '^';
            if (invert)
                ++at_;
            bool any = false;
            while (at_ < input_.size() && input_[at_] != ']') {
                std::array<uint64_t, 4> first{};
                auto q = literal();
                if (q == '\\')
                    first = escaped();
                else
                    set(first, q);
                if (at_ + 1 < input_.size() && input_[at_] == '-' && input_[at_ + 1] != ']') {
                    ++at_;
                    auto end = literal();
                    if (end == '\\') {
                        auto b = escaped();
                        unsigned found = 256;
                        for (unsigned i = 0; i < 256; ++i)
                            if (has(b, i)) {
                                require(found == 256, FQDB_INVALID_ARGUMENT);
                                found = i;
                            }
                        end = found;
                    }
                    unsigned begin = 256;
                    for (unsigned i = 0; i < 256; ++i)
                        if (has(first, i)) {
                            require(begin == 256, FQDB_INVALID_ARGUMENT);
                            begin = i;
                        }
                    require(begin <= end && end < 256, FQDB_INVALID_ARGUMENT);
                    for (unsigned i = begin; i <= end; ++i)
                        set(n.bits, i);
                } else
                    for (unsigned i = 0; i < 4; ++i)
                        n.bits[i] |= first[i];
                any = true;
            }
            require(any && at_ < input_.size() && input_[at_++] == ']', FQDB_INVALID_ARGUMENT);
            // Fold the positive class before inversion, preserving [^a] under /i.
            if (insensitive_)
                for (unsigned i = 'A'; i <= 'Z'; ++i)
                    if (has(n.bits, i) || has(n.bits, i + 32)) {
                        set(n.bits, i);
                        set(n.bits, i + 32);
                    }
            if (invert)
                for (auto &w : n.bits)
                    w = ~w;
            return node(n);
        } else {
            require(c != '*' && c != '+' && c != '?' && c != '{' && c != '}' && c != ')' &&
                        c != '|',
                    FQDB_INVALID_ARGUMENT);
            set(n.bits, c);
        }
        if (insensitive_)
            for (unsigned i = 'A'; i <= 'Z'; ++i)
                if (has(n.bits, i) || has(n.bits, i + 32)) {
                    set(n.bits, i);
                    set(n.bits, i + 32);
                }
        return node(n);
    }
    int sequence(unsigned depth) {
        std::vector<int> result;
        while (at_ < input_.size() && input_[at_] != ')' && input_[at_] != '|') {
            int a = atom(depth);
            if (at_ < input_.size()) {
                auto c = input_[at_];
                Node repeat;
                repeat.kind = 4;
                repeat.a = a;
                if (c == '*' || c == '+' || c == '?') {
                    ++at_;
                    repeat.minimum = c == '+' ? 1 : 0;
                    repeat.maximum = c == '?' ? 1 : -1;
                    a = node(repeat);
                } else if (c == '{') {
                    ++at_;
                    auto number = [&]() {
                        unsigned n = 0, count = 0;
                        while (at_ < input_.size() && input_[at_] >= '0' && input_[at_] <= '9') {
                            n = n * 10 + unsigned(input_[at_++] - '0');
                            require(n <= 32, FQDB_LIMIT);
                            ++count;
                        }
                        require(count, FQDB_INVALID_ARGUMENT);
                        return int(n);
                    };
                    repeat.minimum = number();
                    repeat.maximum = repeat.minimum;
                    if (at_ < input_.size() && input_[at_] == ',') {
                        ++at_;
                        repeat.maximum = at_ < input_.size() && input_[at_] == '}' ? -1 : number();
                    }
                    require(at_ < input_.size() && input_[at_++] == '}' &&
                                (repeat.maximum < 0 || repeat.maximum >= repeat.minimum),
                            FQDB_INVALID_ARGUMENT);
                    a = node(repeat);
                }
                if (at_ < input_.size())
                    require(input_[at_] != '*' && input_[at_] != '+' && input_[at_] != '?' &&
                                input_[at_] != '{',
                            FQDB_INVALID_ARGUMENT);
            }
            result.push_back(a);
        }
        return balanced(std::move(result), 2);
    }
    int alternation(unsigned depth) {
        std::vector<int> v;
        v.push_back(sequence(depth));
        while (at_ < input_.size() && input_[at_] == '|') {
            ++at_;
            v.push_back(sequence(depth));
        }
        return balanced(std::move(v), 3);
    }
    int state(int kind, int a = -1, int b = -1, std::array<uint64_t, 4> bits = {}) {
        if (compiler_)
            compiler_->tick();
        require(states_.size() < 2048, FQDB_LIMIT);
        states_.push_back({kind, a, b, bits});
        return int(states_.size() - 1);
    }
    void patch(const std::vector<std::pair<int, bool>> &ends, int target) {
        for (auto p : ends)
            (p.second ? states_[p.first].b : states_[p.first].a) = target;
    }
    Fragment concat(Fragment a, Fragment b) {
        patch(a.ends, b.start);
        return {a.start, std::move(b.ends)};
    }
    Fragment compile(int at, unsigned depth) {
        require(depth <= 128, FQDB_LIMIT);
        auto n = nodes_[at];
        if (n.kind == 2)
            return concat(compile(n.a, depth + 1), compile(n.b, depth + 1));
        if (n.kind == 3) {
            auto a = compile(n.a, depth + 1), b = compile(n.b, depth + 1);
            auto s = state(3, a.start, b.start);
            a.ends.insert(a.ends.end(), b.ends.begin(), b.ends.end());
            return {s, std::move(a.ends)};
        }
        if (n.kind == 4) {
            int empty = state(0);
            Fragment out{empty, {{empty, false}}};
            for (int i = 0; i < n.minimum; ++i)
                out = concat(std::move(out), compile(n.a, depth + 1));
            if (n.maximum < 0) {
                auto item = compile(n.a, depth + 1);
                int s = state(3, item.start);
                patch(item.ends, s);
                out = concat(std::move(out), {s, {{s, true}}});
            } else
                for (int i = n.minimum; i < n.maximum; ++i) {
                    auto item = compile(n.a, depth + 1);
                    int s = state(3, item.start);
                    item.ends.push_back({s, true});
                    out = concat(std::move(out), {s, std::move(item.ends)});
                }
            return out;
        }
        int s = state(n.kind == 1 ? 1 : n.kind == 5 ? 5 : n.kind == 6 ? 6 : 0, -1, -1, n.bits);
        return {s, {{s, false}}};
    }

  public:
    struct Scratch {
        Vec<int> current, next, stack;
        Vec<uint32_t> seen;
        uint32_t epoch = 0;
        explicit Scratch(Memory &m)
            : current(Alloc<int>(m)), next(Alloc<int>(m)), stack(Alloc<int>(m)),
              seen(Alloc<uint32_t>(m)) {}
    };
    explicit Pattern(Memory &memory, std::string_view pattern, bool insensitive = false,
                     bool like = false, Work *compiler = nullptr)
        : nodes_(Alloc<Node>(memory)), states_(Alloc<State>(memory)), insensitive_(insensitive),
          compiler_(compiler) {
        require(pattern.size() <= 1024, FQDB_LIMIT);
        std::string translated;
        if (like) {
            translated = '^';
            for (size_t i = 0; i < pattern.size(); ++i) {
                auto c = pattern[i];
                bool escape = c == '\\';
                if (escape) {
                    require(++i < pattern.size(), FQDB_INVALID_ARGUMENT);
                    c = pattern[i];
                }
                if (!escape && c == '%')
                    translated += "[\\x00-\\xff]*";
                else if (!escape && c == '_')
                    translated +=
                        "(?:[\\x00-\\x7f]|[\\xc2-\\xdf][\\x80-\\xbf]|\\xe0[\\xa0-\\xbf][\\x80-"
                        "\\xbf]|[\\xe1-\\xec\\xee-\\xef][\\x80-\\xbf]{2}|\\xed[\\x80-\\x9f][\\x80-"
                        "\\xbf]|\\xf0[\\x90-\\xbf][\\x80-\\xbf]{2}|[\\xf1-\\xf3][\\x80-\\xbf]{3}|"
                        "\\xf4[\\x80-\\x8f][\\x80-\\xbf]{2})";
                else {
                    if (std::string_view("\\.^$|?*+()[]{}").find(c) != std::string_view::npos)
                        translated += '\\';
                    translated += c;
                }
            }
            translated += '$';
            input_ = translated;
        } else
            input_ = pattern;
        root_ = alternation(0);
        require(at_ == input_.size(), FQDB_INVALID_ARGUMENT);
        auto f = compile(root_, 0);
        int end = state(2);
        patch(f.ends, end);
        start_ = f.start;
        input_ = {};
        nodes_.clear();
        nodes_.shrink_to_fit();
        compiler_ = nullptr;
    }
    bool matches(std::string_view input, Work &w, Scratch &s) const {
        w.tick(states_.size());
        s.current.clear();
        s.next.clear();
        s.stack.clear();
        s.seen.assign(states_.size(), 0);
        s.epoch = 1;
        auto add = [&](Vec<int> &out, int first, size_t position) {
            auto push = [&](int i) {
                require(i >= 0 && size_t(i) < states_.size());
                if (s.seen[size_t(i)] != s.epoch) {
                    s.seen[size_t(i)] = s.epoch;
                    s.stack.push_back(i);
                }
            };
            push(first);
            while (!s.stack.empty()) {
                int i = s.stack.back();
                s.stack.pop_back();
                w.tick();
                const auto &n = states_[size_t(i)];
                if (n.kind == 0)
                    push(n.a);
                else if (n.kind == 3) {
                    push(n.a);
                    push(n.b);
                } else if (n.kind == 5) {
                    if (!position)
                        push(n.a);
                } else if (n.kind == 6) {
                    if (position == input.size())
                        push(n.a);
                } else
                    out.push_back(i);
            }
        };
        for (size_t p = 0;; ++p) {
            add(s.current, start_, p);
            for (int i : s.current)
                if (states_[size_t(i)].kind == 2)
                    return true;
            if (p == input.size())
                return false;
            ++s.epoch;
            if (!s.epoch) {
                std::fill(s.seen.begin(), s.seen.end(), 0);
                s.epoch = 1;
            }
            s.next.clear();
            auto c = static_cast<unsigned char>(input[p]);
            for (int i : s.current) {
                w.tick();
                const auto &n = states_[size_t(i)];
                if (n.kind == 1 && has(n.bits, c))
                    add(s.next, n.a, p + 1);
            }
            s.current.swap(s.next);
        }
    }
};
} // namespace fengqudb::paged::select

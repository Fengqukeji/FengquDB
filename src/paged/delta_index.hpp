#pragma once
#include "page.hpp"
#include <optional>

namespace fengqudb::paged {
struct DeltaNode;
using DeltaRoot = std::shared_ptr<const DeltaNode>;
struct DeltaNode {
    DeltaRoot left, right;
    detail::Key key;
    Ref value;
    uint64_t count = 0, bytes = 0, maximum = 0, earliest = UINT64_MAX;
    DeltaNode(detail::Key k, Ref v, DeltaRoot l = {}, DeltaRoot r = {})
        : left(std::move(l)), right(std::move(r)), key(std::move(k)), value(v) {
        update();
    }
    void update() noexcept {
        key.height = static_cast<uint8_t>(
            1 + std::max(left ? left->key.height : 0, right ? right->key.height : 0));
        count = 1 + (left ? left->count : 0) + (right ? right->count : 0);
        bytes = sizeof(DeltaNode) + 32 + (key.external ? key.external->capacity() + 64 : 0) +
                (left ? left->bytes : 0) + (right ? right->bytes : 0);
        maximum =
            std::max(value.version, std::max(left ? left->maximum : 0, right ? right->maximum : 0));
        earliest =
            std::min(value.earliest_expiry(), std::min(left ? left->earliest : UINT64_MAX,
                                                       right ? right->earliest : UINT64_MAX));
    }
};
inline const DeltaNode *delta_find(const DeltaRoot &root, std::string_view key) noexcept {
    auto node = root.get();
    while (node) {
        auto order = compare(key, node->key.view());
        if (!order)
            return node;
        node = (order < 0 ? node->left : node->right).get();
    }
    return nullptr;
}
// The published root stays pinned while a private batch edits this tree. Each
// shared path is copied once; subsequent changes reuse its private nodes.
class DeltaBuilder {
    DeltaRoot root_;
    static DeltaNode *mut(const DeltaRoot &root) noexcept {
        return const_cast<DeltaNode *>(root.get());
    }
    static void own(DeltaRoot &root) {
        if (root && root.use_count() != 1)
            root = std::make_shared<DeltaNode>(root->key, root->value, root->left, root->right);
    }
    static int height(const DeltaRoot &root) noexcept {
        return root ? root->key.height : 0;
    }
    static void left(DeltaRoot &root) {
        own(mut(root)->right);
        auto old = std::move(root);
        auto node = mut(old);
        root = std::move(node->right);
        auto next = mut(root);
        node->right = std::move(next->left);
        node->update();
        next->left = std::move(old);
        next->update();
    }
    static void right(DeltaRoot &root) {
        own(mut(root)->left);
        auto old = std::move(root);
        auto node = mut(old);
        root = std::move(node->left);
        auto next = mut(root);
        node->left = std::move(next->right);
        node->update();
        next->right = std::move(old);
        next->update();
    }
    static void insert(DeltaRoot &root, const Bytes &key, Ref value) {
        if (!root) {
            root = std::make_shared<DeltaNode>(detail::Key(key), value);
            return;
        }
        own(root);
        auto node = mut(root);
        auto order = compare(key, node->key.view());
        if (order < 0)
            insert(node->left, key, value);
        else if (order > 0)
            insert(node->right, key, value);
        else
            node->value = value;
        node->update();
        auto balance = height(node->left) - height(node->right);
        if (balance > 1) {
            if (height(node->left->left) < height(node->left->right))
                left(node->left);
            right(root);
        } else if (balance < -1) {
            if (height(node->right->right) < height(node->right->left))
                right(node->right);
            left(root);
        }
    }

  public:
    explicit DeltaBuilder(DeltaRoot root = {}) : root_(std::move(root)) {}
    void set(const Bytes &key, Ref value) {
        insert(root_, key, value);
    }
    DeltaRoot finish() noexcept {
        return std::move(root_);
    }
};
class DeltaCursor {
    DeltaRoot root_;
    std::vector<const DeltaNode *> stack_;
    bool reverse_;
    uint64_t since_;
    void descend(const DeltaNode *node) {
        while (node && node->maximum > since_) {
            stack_.push_back(node);
            node = (reverse_ ? node->right : node->left).get();
        }
    }

  public:
    explicit DeltaCursor(DeltaRoot root, std::optional<std::string_view> begin = {},
                         bool reverse = false, uint64_t since = 0)
        : root_(std::move(root)), reverse_(reverse), since_(since) {
        stack_.reserve(root_ ? root_->key.height + 1 : 1);
        auto node = root_.get();
        while (node && node->maximum > since_) {
            auto order = begin ? compare(node->key.view(), *begin) : 0;
            if (!begin || (reverse_ ? order <= 0 : order >= 0)) {
                stack_.push_back(node);
                node = (reverse_ ? node->right : node->left).get();
            } else
                node = (reverse_ ? node->left : node->right).get();
        }
    }
    const DeltaNode *next() {
        while (!stack_.empty()) {
            auto result = stack_.back();
            stack_.pop_back();
            descend((reverse_ ? result->left : result->right).get());
            if (result->value.version > since_)
                return result;
        }
        return nullptr;
    }
};
inline uint64_t delta_range_max(const DeltaRoot &root, std::string_view begin, std::string_view end,
                                std::string_view lower = {}, std::string_view upper = {}) noexcept {
    if (!root)
        return 0;
    if ((begin.empty() || (!lower.empty() && compare(begin, lower) <= 0)) &&
        (end.empty() || (!upper.empty() && compare(end, upper) >= 0)))
        return root->maximum;
    const auto key = root->key.view();
    if (!begin.empty() && compare(key, begin) < 0)
        return delta_range_max(root->right, begin, end, key, upper);
    if (!end.empty() && compare(key, end) >= 0)
        return delta_range_max(root->left, begin, end, lower, key);
    return std::max(root->value.version,
                    std::max(delta_range_max(root->left, begin, end, lower, key),
                             delta_range_max(root->right, begin, end, key, upper)));
}
} // namespace fengqudb::paged

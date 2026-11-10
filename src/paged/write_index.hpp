#pragma once
#include "budget.hpp"
#include "wal.hpp"

namespace fengqudb::paged {
struct Write {
    OpKind kind = OpKind::erase;
    bool edited = false;
    std::shared_ptr<StagedData> value, insert;
    Ref original;
    uint64_t expires = 0, offset = 0, remove = 0;
};
struct SingleWrite {
    detail::Key key;
    Write value;
    std::shared_ptr<Budget> budget, global;
    Charge charge;
    bool changed = false;
    SingleWrite(const Bytes &name, uint64_t limit, std::shared_ptr<Budget> all)
        : key(name), budget(std::make_shared<Budget>(limit)), global(std::move(all)),
          charge(budget, global, sizeof(SingleWrite) + name.size() + 64) {}
    auto data(const Data &value) {
        return staged_data(value, budget, global);
    }
    auto data(std::string_view value) {
        return staged_data(value, budget, global);
    }
};
class SingleWriteCursor {
    const SingleWrite *value_;

  public:
    explicit SingleWriteCursor(const SingleWrite &value)
        : value_(value.changed ? &value : nullptr) {}
    const SingleWrite *next() {
        return std::exchange(value_, nullptr);
    }
};
struct WriteNode;
using WriteRoot = std::shared_ptr<const WriteNode>;
struct WriteNode {
    Charge charge;
    detail::Key key;
    Write value;
    WriteRoot left, right;
    uint64_t count = 1;
    WriteNode(detail::Key k, Write v, std::shared_ptr<Budget> local, std::shared_ptr<Budget> global,
              WriteRoot l = {}, WriteRoot r = {})
        : charge(std::move(local), std::move(global), sizeof(WriteNode) + k.size + 128),
          key(std::move(k)), value(std::move(v)), left(std::move(l)), right(std::move(r)) {
        update();
    }
    void update() noexcept {
        key.height = static_cast<uint8_t>(
            1 + std::max(left ? left->key.height : 0, right ? right->key.height : 0));
        count = 1 + (left ? left->count : 0) + (right ? right->count : 0);
    }
};
inline const WriteNode *write_find(const WriteRoot &root, std::string_view key) noexcept {
    auto node = root.get();
    while (node) {
        auto order = compare(key, node->key.view());
        if (!order)
            return node;
        node = (order < 0 ? node->left : node->right).get();
    }
    return nullptr;
}
class WriteBuilder {
    WriteRoot root_;
    std::shared_ptr<Budget> local_, global_;
    static WriteNode *mut(const WriteRoot &root) noexcept {
        return const_cast<WriteNode *>(root.get());
    }
    void own(WriteRoot &root) {
        if (root && root.use_count() != 1)
            root = std::make_shared<WriteNode>(root->key, root->value, local_, global_, root->left,
                                               root->right);
    }
    static int height(const WriteRoot &root) noexcept {
        return root ? root->key.height : 0;
    }
    void left(WriteRoot &root) {
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
    void right(WriteRoot &root) {
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
    void insert(WriteRoot &root, const Bytes &key, Write value) {
        if (!root) {
            root = std::make_shared<WriteNode>(detail::Key(key), std::move(value), local_, global_);
            return;
        }
        own(root);
        auto node = mut(root);
        auto order = compare(key, node->key.view());
        if (order < 0)
            insert(node->left, key, std::move(value));
        else if (order > 0)
            insert(node->right, key, std::move(value));
        else
            node->value = std::move(value);
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
    WriteBuilder(WriteRoot root, std::shared_ptr<Budget> local, std::shared_ptr<Budget> global)
        : root_(std::move(root)), local_(std::move(local)), global_(std::move(global)) {}
    // Own the search path before examining use_count of a staged value. A
    // savepoint can share a node without another shared_ptr to its value.
    Write *editable(const Bytes &key) {
        auto root = &root_;
        while (*root) {
            own(*root);
            auto node = mut(*root);
            auto order = compare(key, node->key.view());
            if (!order)
                return &node->value;
            root = order < 0 ? &node->left : &node->right;
        }
        return nullptr;
    }
    void set(const Bytes &key, Write value) {
        insert(root_, key, std::move(value));
    }
    WriteRoot finish() noexcept {
        return std::move(root_);
    }
};
class WriteCursor {
    WriteRoot root_;
    std::array<const WriteNode *, 64> stack_{};
    size_t size_ = 0;
    bool reverse_;
    void descend(const WriteNode *node) {
        while (node) {
            require(size_ < stack_.size());
            stack_[size_++] = node;
            node = (reverse_ ? node->right : node->left).get();
        }
    }

  public:
    explicit WriteCursor(WriteRoot root, std::optional<std::string_view> begin = {},
                         bool reverse = false)
        : root_(std::move(root)), reverse_(reverse) {
        auto node = root_.get();
        while (node) {
            auto order = begin ? compare(node->key.view(), *begin) : 0;
            if (!begin || (reverse_ ? order <= 0 : order >= 0)) {
                require(size_ < stack_.size());
                stack_[size_++] = node;
                node = (reverse_ ? node->right : node->left).get();
            } else
                node = (reverse_ ? node->left : node->right).get();
        }
    }
    const WriteNode *next() {
        if (!size_)
            return nullptr;
        auto node = stack_[--size_];
        descend((reverse_ ? node->left : node->right).get());
        return node;
    }
};
} // namespace fengqudb::paged

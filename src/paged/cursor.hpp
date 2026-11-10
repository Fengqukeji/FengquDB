#pragma once
#include "core.hpp"

namespace fengqudb::paged {
class ReadCursor {
    std::shared_ptr<Capture> base_;
    Charge pins_;
    ViewCursor disk_;
    WriteCursor writes_;
    Bytes begin_, end_;
    std::optional<Metadata> row_;
    const WriteNode *write_ = nullptr;
    bool advance_disk_ = true, advance_write_ = true, reverse_, ended_ = false, broken_ = false,
         internal_ = false, hide_system_ = false;
    Value value_;

  public:
    const Value &value_ref() const noexcept {
        return value_;
    }
    ReadCursor(std::shared_ptr<Capture> base, Bytes begin, Bytes end, bool reverse,
               WriteRoot writes = {}, bool internal = false, bool hide_system = false)
        : base_(std::move(base)),
          pins_(base_->core->pin_budget, {}, sizeof(ReadCursor) + begin.size() + end.size() + 256),
          disk_(*base_->view,
                reverse && !end.empty() ? std::optional<std::string_view>(end)
                                        : (reverse ? std::optional<std::string_view>{}
                                                   : std::optional<std::string_view>(begin)),
                reverse),
          writes_(std::move(writes),
                  reverse && !end.empty() ? std::optional<std::string_view>(end)
                                          : (reverse ? std::optional<std::string_view>{}
                                                     : std::optional<std::string_view>(begin)),
                  reverse),
          begin_(std::move(begin)), end_(std::move(end)), reverse_(reverse), internal_(internal),
          hide_system_(hide_system || (base_->view->checkpoint.features & 1)) {
        require(end_.empty() || compare(begin_, end_) <= 0, FQDB_INVALID_ARGUMENT);
    }
    std::optional<fqdb_store_row> next(bool load_value = true) {
        require(!broken_, FQDB_INVALID_ARGUMENT);
        if (ended_)
            return {};
        try {
            value_.reset();
            // Reuse only a bounded reservation while finding the next value.
            // Large payloads relinquish their pins before any I/O.
            if (pins_.bytes() > sizeof(ReadCursor) + begin_.size() + end_.size() + 256 + 65536)
                pins_.resize(sizeof(ReadCursor) + begin_.size() + end_.size() + 256);
            for (;;) {
                if (advance_disk_)
                    row_ = disk_.next();
                if (advance_write_)
                    write_ = writes_.next();
                advance_disk_ = advance_write_ = false;
                if (!row_ && !write_) {
                    ended_ = true;
                    pins_.resize(sizeof(ReadCursor) + begin_.size() + end_.size() + 256);
                    return {};
                }
                auto order = !row_ ? 1 : (!write_ ? -1 : compare(row_->key, write_->key.view()));
                const bool staged = write_ && (!row_ || (reverse_ ? order <= 0 : order >= 0));
                std::string_view key;
                uint64_t version = 0, expires = 0;
                bool visible;
                if (staged) {
                    key = write_->key.view();
                    const auto &value = write_->value;
                    expires = value.expires;
                    visible = value.kind != OpKind::erase && (!expires || expires > base_->time);
                    advance_write_ = true;
                    advance_disk_ = row_ && !order;
                } else {
                    key = row_->key;
                    version = row_->value.version;
                    expires = row_->value.expires;
                    visible = row_->value.visible(base_->time);
                    advance_disk_ = true;
                }
                if ((!reverse_ && !end_.empty() && compare(key, end_) >= 0) ||
                    (reverse_ && compare(key, begin_) < 0)) {
                    ended_ = true;
                    pins_.resize(sizeof(ReadCursor) + begin_.size() + end_.size() + 256);
                    return {};
                }
                if ((!internal_ && hide_system_ && system_key(key)) || !visible ||
                    compare(key, begin_) < 0 || (!end_.empty() && compare(key, end_) >= 0))
                    continue;
                if (!load_value) {
                    pins_.resize(sizeof(ReadCursor) + begin_.size() + end_.size() + 256);
                    return fqdb_store_row{{key.data(), key.size()}, {nullptr, 0}, version, expires};
                }
                if (staged)
                    value_ = write_->value.value ? Value(write_->value.value)
                                                 : base_->view->generation->value(
                                                       write_->value.original, base_->view->limit);
                else
                    value_ = base_->view->generation->value(row_->value, base_->view->limit);
                pins_.resize(sizeof(ReadCursor) + begin_.size() + end_.size() + 256 + sizeof(Data) +
                             value_->capacity());
                return fqdb_store_row{
                    {key.data(), key.size()}, {value_->data(), value_->size()}, version, expires};
            }
        } catch (...) {
            broken_ = true;
            value_.reset();
            pins_.resize(sizeof(ReadCursor) + begin_.size() + end_.size() + 256);
            throw;
        }
    }
};
} // namespace fengqudb::paged

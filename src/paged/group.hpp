#pragma once
#ifndef FQDB_CORE_IMPLEMENTATION
#error "group.hpp is an implementation fragment owned by store.cpp"
#endif
#include "core.hpp"

namespace fengqudb::paged {
inline fqdb_status current_error() noexcept {
    try {
        throw;
    } catch (const Error &error) {
        return error.status;
    } catch (const std::bad_alloc &) {
        return FQDB_NO_MEMORY;
    } catch (const std::length_error &) {
        return FQDB_LIMIT;
    } catch (...) {
        return FQDB_INTERNAL_ERROR;
    }
}
uint64_t Core::commit_direct(CommitWaiter &request) {
    request.reliable = request.force_reliable || options.durability == FQDB_DURABILITY_SYNC;
    // BUFFERED has no flush to share. Optional metered maintenance also stays
    // on its caller thread, so unrelated grouped writes never spend its quota.
    std::unique_lock<WriterGate> lock(writer, std::defer_lock);
    if (!lock.try_lock())
        for (;;) {
            request.ready();
            if (request.transaction)
                request.transaction->ready();
            if (lock.try_lock_for(std::chrono::milliseconds(2)))
                break;
        }
    return commit_locked(request);
}
uint64_t Core::commit_locked(CommitWaiter &request) {
    request.ready();
    if (request.transaction)
        request.transaction->ready();
    require(!poisoned, FQDB_RECOVERY_REQUIRED);
    sync_business_locked();
    checkpoint_locked_if_needed();
    try {
        auto next = append_locked(request, current());
        // The WAL has finished borrowing staged input before private cleanup.
        request.release_private();
        if (request.appended) {
            if (request.reliable) {
                maintenance_at(40);
                if (request.trace)
                    request.trace->stage = FQDB_STAGE_FLUSH;
                try {
                    next->generation->file->sync();
                } catch (...) {
                    poisoned = true;
                    throw Error(FQDB_COMMIT_UNKNOWN);
                }
                auto durable = std::const_pointer_cast<View>(next);
                durable->durable = durable->version;
                group_flushes.fetch_add(1, std::memory_order_relaxed);
                dirty_bytes.store(0, std::memory_order_relaxed);
            }
            store_crash_at(3);
            publish(std::move(next));
            store_crash_at(4);
            group_transactions.fetch_add(1, std::memory_order_relaxed);
        }
        request.finish();
        if (request.trace) {
            request.trace->version = request.version;
            request.trace->stage = request.appended ? FQDB_STAGE_PUBLISH : FQDB_STAGE_INDEX;
        }
        return request.version;
    } catch (const Error &error) {
        if (error.status == FQDB_COMMIT_UNKNOWN)
            request.finish();
        request.release_private();
        throw;
    } catch (...) {
        request.release_private();
        throw;
    }
}
#include "buffered_submit.hpp"

uint64_t Core::submit_strict(CommitWaiter &request) {
    std::unique_lock<std::mutex> lock(group_mutex);
    require(group_count < 1024, FQDB_LIMIT);
    if (group_tail)
        group_tail->next = &request;
    else
        group_head = &request;
    group_tail = &request;
    ++group_count;
    if (group_active && group_delay_us.load(std::memory_order_relaxed))
        group_changed.notify_all();
    while (!request.done) {
        if (!group_active && group_head == &request) {
            group_active = true;
            lock.unlock();
            drain_group();
            lock.lock();
        } else
            group_changed.wait(lock);
    }
    auto status = request.status;
    lock.unlock();
    require(status == FQDB_OK, status);
    return request.version;
}
template <class Take>
size_t Core::process_group(CommitWaiter **batch, size_t maximum, Take &&take, bool reliable) {
    size_t count = 0;
    std::shared_ptr<const View> pending;
    bool wrote = false;
    fqdb_status failure = FQDB_OK;
    try {
        std::lock_guard<WriterGate> lock(writer);
        if (!poisoned)
            checkpoint_locked_if_needed();
        pending = current();
        for (; count < maximum;) {
            auto request = take();
            if (!request)
                break;
            batch[count++] = request;
            try {
                pending = append_locked(*request, pending);
                wrote |= request->appended;
                if (!reliable && request->appended)
                    publish(pending);
            } catch (...) {
                request->status = current_error();
                if (poisoned)
                    failure = FQDB_COMMIT_UNKNOWN;
            }
            request->release_private();
            if (failure)
                break;
            // Fold only a published, durable view between groups, never an
            // unpublished pending root while readers still see its predecessor.
            if (pending->delta && pending->delta->bytes > options.delta_bytes / 2)
                break;
            if (!reliable &&
                dirty_bytes.load(std::memory_order_relaxed) >= write_policy.threshold_bytes)
                break;
        }
        if (reliable && !failure && (wrote || pending->durable < pending->version)) {
            try {
                {
                    auto durable = wrote ? std::const_pointer_cast<View>(pending)
                                         : std::make_shared<View>(*pending);
                    maintenance_at(40);
                    for (size_t i = 0; i < count; ++i)
                        if (batch[i]->appended && batch[i]->trace)
                            batch[i]->trace->stage = FQDB_STAGE_FLUSH;
                    pending->generation->file->sync();
                    durable->durable = durable->version;
                    pending = std::move(durable);
                    group_flushes.fetch_add(1, std::memory_order_relaxed);
                    dirty_bytes.store(0, std::memory_order_relaxed);
                }
                store_crash_at(3);
                publish(pending);
                store_crash_at(4);
            } catch (...) {
                poisoned = true;
                failure = FQDB_COMMIT_UNKNOWN;
                write_failure.store(failure, std::memory_order_release);
            }
        }
    } catch (...) {
        failure = current_error();
    }
    if (poisoned)
        write_failure.store(FQDB_COMMIT_UNKNOWN, std::memory_order_release);
    // Even checkpoint/allocation failures must detach a waiter and relinquish
    // leadership, otherwise a queue head could spin or wait forever.
    if (!count) {
        if (auto request = take())
            batch[count++] = request;
    }
    for (size_t i = 0; i < count; ++i) {
        auto &request = *batch[i];
        if (request.appended) {
            if (failure && reliable)
                request.status = FQDB_COMMIT_UNKNOWN;
            request.finish();
            group_transactions.fetch_add(1, std::memory_order_relaxed);
        } else if (!request.status && failure)
            request.status = failure;
        else if (!request.status)
            request.finish();
        if (request.status == FQDB_COMMIT_UNKNOWN)
            request.finish();
        request.release_private();
        if (request.trace) {
            request.trace->version = request.version;
            if (!request.status)
                request.trace->stage = request.appended ? FQDB_STAGE_PUBLISH : FQDB_STAGE_INDEX;
        }
    }
    return count;
}
void Core::drain_group() {
    auto delay = group_delay_us.load(std::memory_order_relaxed);
    if (delay) {
        std::unique_lock<std::mutex> lock(group_mutex);
        group_changed.wait_for(lock, std::chrono::microseconds(delay), [&] {
            return group_count >= group_max.load(std::memory_order_relaxed);
        });
    }
    std::array<CommitWaiter *, 256> batch{};
    const auto count = process_group(batch.data(), group_max.load(std::memory_order_relaxed), [&] {
        std::lock_guard<std::mutex> queue(group_mutex);
        auto request = group_head;
        if (request) {
            group_head = request->next;
            if (!group_head)
                group_tail = nullptr;
            --group_count;
        }
        return request;
    });
    {
        std::lock_guard<std::mutex> lock(group_mutex);
        for (size_t i = 0; i < count; ++i)
            batch[i]->done = true;
        group_active = false;
    }
    group_changed.notify_all();
}
#include "periodic_write.hpp"
} // namespace fengqudb::paged

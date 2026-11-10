#pragma once

// Included inside fengqudb::paged. Delegation combines execution only:
// every transaction keeps its own WAL record, publication and completion.
inline void buffered_unlink(Core &core, CommitWaiter &request) {
    CommitWaiter *previous = nullptr;
    auto cursor = core.group_head;
    while (cursor && cursor != &request) {
        previous = cursor;
        cursor = cursor->next;
    }
    require(cursor == &request, FQDB_INTERNAL_ERROR);
    if (previous)
        previous->next = request.next;
    else
        core.group_head = request.next;
    if (core.group_tail == &request)
        core.group_tail = previous;
    --core.group_count;
    request.queued = false;
    request.next = nullptr;
}

uint64_t Core::commit_buffered(CommitWaiter &request) {
    request.ready();
    if (request.transaction)
        request.transaction->ready();
    // Keep the original direct submission when there is no actual contention.
    // A contender arriving after this check can become the next delegate while
    // waiting for this lease; new direct callers then observe its active flag.
    if (!buffered_active.load(std::memory_order_acquire) && writer.try_lock()) {
        std::unique_lock<WriterGate> direct(writer, std::adopt_lock);
        return commit_locked(request);
    }
    uint8_t inactive = 0;
    if (!buffered_active.compare_exchange_strong(inactive, 1,
                                                 std::memory_order_acquire)) {
        std::unique_lock<std::mutex> queue(group_mutex);
        // Set the pending bit BEFORE linking under the queue mutex. A leader
        // can then release an empty fast path by CAS(1, 0), without a queue lock
        // and without losing a request that is about to become visible.
        const auto state = buffered_active.fetch_or(2, std::memory_order_acq_rel);
        if (!state)
            buffered_active.store(1, std::memory_order_relaxed);
        else {
            request.completion.emplace();
            require(group_count < 1024, FQDB_LIMIT);
            if (group_tail)
                group_tail->next = &request;
            else
                group_head = &request;
            group_tail = &request;
            request.queued = true;
            ++group_count;
            while (!request.done && !request.promoted) {
                // Once detached, the leader owns all access to transaction
                // state until completion. Only queued requests may withdraw.
                if (request.queued)
                    try {
                        request.ready();
                        if (request.transaction)
                            request.transaction->ready();
                    } catch (...) {
                        buffered_unlink(*this, request);
                        throw;
                    }
                request.completion->wait_for(queue, std::chrono::milliseconds(2));
            }
            if (request.done) {
                require(request.status == FQDB_OK, request.status);
                return request.version;
            }
        }
    }
    drain_buffered(request);
    require(request.status == FQDB_OK, request.status);
    return request.version;
}

void Core::drain_buffered(CommitWaiter &leader) {
    std::unique_lock<WriterGate> lease(writer, std::defer_lock);
    CommitWaiter *request = &leader;
    try {
        if (!lease.try_lock())
            for (;;) {
                leader.ready();
                if (leader.transaction)
                    leader.transaction->ready();
                if (lease.try_lock_for(std::chrono::milliseconds(2)))
                    break;
            }
        try {
            commit_locked(leader);
        } catch (...) {
            leader.status = current_error();
            leader.release_private();
        }
        request = nullptr;
        uint8_t alone = 1;
        if (buffered_active.compare_exchange_strong(alone, 0,
                                                    std::memory_order_release))
            return; // No queue mutex, clock or condition variable on this path.
        const auto start = std::chrono::steady_clock::now();
        const auto maximum = std::min<uint32_t>(64, group_max.load(std::memory_order_relaxed));
        uint64_t bytes = leader.write_bytes;
        uint32_t count = 1;
        for (;;) {
            {
                std::lock_guard<std::mutex> queue(group_mutex);
                if (count >= maximum || bytes >= 256 * 1024 || !group_head ||
                    std::chrono::steady_clock::now() - start >=
                        std::chrono::microseconds(200))
                    break;
                request = group_head;
                buffered_unlink(*this, *request);
            }
            try {
                maintenance_at(73); // Test-only barrier after detachment, before WAL append.
                commit_locked(*request);
            } catch (...) {
                request->status = current_error();
                request->release_private();
            }
            bytes += request->write_bytes;
            ++count;
            std::lock_guard<std::mutex> queue(group_mutex);
            request->done = true;
            // Notify under this mutex: the caller cannot destroy its CV yet.
            request->completion->notify_one();
            request = nullptr;
        }
    } catch (...) {
        const auto status = current_error();
        if (request) {
            request->status = status;
            request->release_private();
            if (request != &leader) {
                std::lock_guard<std::mutex> queue(group_mutex);
                request->done = true;
                request->completion->notify_one();
            }
        } else
            leader.status = status;
    }
    if (lease.owns_lock())
        lease.unlock();
    std::lock_guard<std::mutex> queue(group_mutex);
    if (group_head) {
        auto next = group_head;
        buffered_unlink(*this, *next);
        // Release the pending bit when this promotion drains the queue. New
        // enqueuers set it again under this same mutex before linking.
        buffered_active.store(group_head ? 3 : 1, std::memory_order_relaxed);
        next->promoted = true;
        next->completion->notify_one();
    } else
        buffered_active.store(0, std::memory_order_release);
}

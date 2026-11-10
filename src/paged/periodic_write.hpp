#pragma once
#ifndef FQDB_CORE_IMPLEMENTATION
#error "periodic_write.hpp is an implementation fragment owned by store.cpp"
#endif
// Included inside fengqudb::paged. Queue state and receipt completion share one
// mutex; synchronous producers acquire results through CommitSignal's atomic.
// Requests stay alive until the selected completion protocol publishes them.
inline uint64_t commit_input_bytes(const CommitWaiter &request) {
    if (request.queued_bytes)
        return request.queued_bytes;
    uint64_t bytes = 64;
    if (request.transaction) {
        WriteCursor cursor(request.transaction->writes);
        while (auto node = cursor.next()) {
            auto value = node->value.kind == OpKind::patch ? node->value.insert : node->value.value;
            auto size = uint64_t(node->key.size) + 64 + (value ? value->size() : 0);
            require(size <= UINT64_MAX - bytes, FQDB_LIMIT);
            bytes += size;
        }
    }
    return bytes;
}
void Core::start_periodic_locked() {
    if (!periodic_thread.joinable()) {
        periodic_stop = false;
        periodic_thread = std::thread([this] { periodic_loop(); });
    }
}
void Core::enqueue_periodic(CommitWaiter &request) {
    if (!request.signal)
        request.completion.emplace(); // Async receipts may have many waiters.
    request.queued_bytes = commit_input_bytes(request);
    std::lock_guard<std::mutex> lock(periodic_mutex);
    require(periodic_sequence != UINT64_MAX && periodic_sequence - periodic_completed < 16384,
            FQDB_LIMIT);
    require(request.queued_bytes <= options.all_transaction_bytes &&
                periodic_pending_bytes <= options.all_transaction_bytes - request.queued_bytes,
            FQDB_LIMIT);
    start_periodic_locked();
    if (request.accept)
        request.accept(request);
    const bool empty = !periodic_head;
    const auto previous = periodic_bytes;
    request.queue_sequence = ++periodic_sequence;
    request.eager_wait = !request.reliable && periodic_sequence - periodic_completed == 1;
    request.next = nullptr;
    if (periodic_tail)
        periodic_tail->next = &request;
    else
        periodic_head = &request;
    periodic_tail = &request;
    ++periodic_count;
    periodic_ready.store(true, std::memory_order_release);
    periodic_bytes += request.queued_bytes;
    periodic_pending_bytes += request.queued_bytes;
    if (empty ||
        (previous < write_policy.threshold_bytes && periodic_bytes >= write_policy.threshold_bytes))
        periodic_changed.notify_one();
}
void Core::strict_finished() noexcept {
    std::lock_guard<std::mutex> lock(admission_mutex);
    --strict_active;
    admission_changed.notify_all();
}
void Core::admit(CommitWaiter &request, bool nonblocking) {
    require(!poisoned.load(std::memory_order_acquire), FQDB_RECOVERY_REQUIRED);
    request.ready();
    if (request.transaction)
        request.transaction->ready();
    std::unique_lock<std::mutex> admission(admission_mutex, std::defer_lock);
    if (nonblocking) {
        // Configuration releases this mutex during disk I/O. Ordinary
        // producers serialize only the short queue-admission section.
        admission.lock();
        require(!reconfiguring, FQDB_LOCKED);
    } else {
        admission.lock();
        admission_changed.wait(admission, [&] { return !reconfiguring; });
    }
    request.reliable = request.force_reliable || (options.durability == FQDB_DURABILITY_SYNC &&
                                                  write_policy.mode == FQDB_WRITE_RELIABLE);
    if (nonblocking ||
        (options.durability == FQDB_DURABILITY_SYNC &&
         write_policy.mode == FQDB_WRITE_PERFORMANCE) ||
        request.force_reliable) {
        enqueue_periodic(request);
        return;
    }
    ++strict_active;
    if (request.accept)
        request.accept(request);
    admission.unlock();
    try {
        if (request.reliable)
            (void)submit_strict(request);
        else {
            const auto root = request.transaction ? request.transaction->writes.get() : nullptr;
            if (root && root->count == 1 && root->value.kind == OpKind::put && root->value.value &&
                root->value.value->size() >= 4096)
                (void)commit_buffered(request);
            else
                (void)commit_direct(request);
        }
    } catch (...) {
        request.status = current_error();
        request.release_private();
    }
    {
        std::lock_guard<std::mutex> lock(periodic_mutex);
        request.done = true;
    }
    strict_finished();
}
bool Core::wait_commit(CommitWaiter &request, uint64_t timeout, bool poll) {
    if (timeout) {
        require(timeout <= uint64_t(INT64_MAX / 1000000), FQDB_INVALID_ARGUMENT);
        require(std::chrono::milliseconds(timeout) <=
                    std::chrono::steady_clock::time_point::max() - std::chrono::steady_clock::now(),
                FQDB_INVALID_ARGUMENT);
    }
    std::unique_lock<std::mutex> lock(periodic_mutex);
    if (poll)
        return request.done;
    if (!request.done) {
        require(request.completion.has_value(), FQDB_INTERNAL_ERROR);
        if (!timeout)
            request.completion->wait(lock, [&] { return request.done; });
        else {
            return request.completion->wait_for(lock, std::chrono::milliseconds(timeout),
                                                [&] { return request.done; });
        }
    }
    return true;
}
uint64_t Core::submit(CommitWaiter &request) {
    // One reusable signal per calling thread avoids per-request CV setup and
    // wakes only its producer; asynchronous receipts retain their
    // existing multi-waiter contract. Nested use gets an independent signal.
    thread_local auto reusable = std::make_shared<CommitSignal>();
    auto signal = reusable->busy ? std::make_shared<CommitSignal>() : reusable;
    signal->busy = true;
    signal->fallback = !CommitSignal::native();
    signal->state.store(0, std::memory_order_relaxed);
    struct Release {
        CommitSignal &value;
        ~Release() {
            value.busy = false;
        }
    } release{*signal};
    request.signal = signal;
    admit(request);
    if (request.queue_sequence && (signal->fallback || !signal->wait_native(request.eager_wait))) {
        std::unique_lock<std::mutex> lock(periodic_mutex);
        signal->fallback = true; // Serialized with notification, including native errors.
        signal->changed.wait(lock, [&] { return request.done; });
    }
    require(request.status == FQDB_OK, request.status);
    return request.version;
}
inline void complete_periodic(CommitWaiter &request) {
    // Retain the signal before publishing completion: a producer can return,
    // reuse its TLS slot or exit as soon as it observes state == 2.
    auto signal = std::move(request.signal);
    request.done = true; // Caller holds periodic_mutex for receipt polling/draining.
    if (signal) {
        if (signal->fallback)
            signal->changed.notify_one();
        else
            signal->complete_native();
    } else
        request.completion->notify_all();
}
void Core::flush_periodic() {
    std::unique_lock<std::mutex> lock(periodic_mutex);
    const auto target = periodic_sequence;
    if (periodic_completed >= target)
        return;
    // Drain accepted requests through target. The worker executes queued work
    // immediately; the caller's sync/configuration path owns disk durability.
    periodic_changed.notify_one();
    periodic_drained.wait(lock, [&] { return periodic_completed >= target; });
}
void Core::cancel_commit(CommitWaiter &request) {
    std::lock_guard<std::mutex> lock(periodic_mutex);
    if (!request.done) {
        // Cancellation is checked by request.ready(); it must not force an
        // unrelated durability barrier for a performance-mode write.
        periodic_changed.notify_one();
    }
}
void Core::stop_periodic() {
    std::unique_lock<std::mutex> admission(admission_mutex);
    admission_changed.wait(admission, [&] { return !reconfiguring; });
    reconfiguring = true;
    admission.unlock();
    flush_periodic();
    {
        std::lock_guard<std::mutex> lock(periodic_mutex);
        periodic_stop = true;
        periodic_changed.notify_one();
    }
    if (periodic_thread.joinable())
        periodic_thread.join();
    admission.lock();
    reconfiguring = false;
    admission_changed.notify_all();
}
fqdb_store_write_options Core::get_write_policy() {
    std::lock_guard<std::mutex> lock(admission_mutex);
    return write_policy;
}
void Core::configure_write(const fqdb_store_write_options &settings) {
    require(valid_write_policy(settings), FQDB_INVALID_ARGUMENT);
    require(!storage::io_quota, FQDB_INVALID_ARGUMENT);
    std::unique_lock<std::mutex> admission(admission_mutex);
    admission_changed.wait(admission, [&] { return !reconfiguring; });
    if (write_policy_saved && write_policy.mode == settings.mode &&
        write_policy.period_ms == settings.period_ms &&
        write_policy.threshold_bytes == settings.threshold_bytes)
        return;
    reconfiguring = true;
    try {
        admission.unlock();
        flush_periodic();
        admission.lock();
        admission_changed.wait(admission, [&] { return strict_active == 0; });
        admission.unlock();
        {
            std::lock_guard<WriterGate> writer_lock(writer);
            sync_locked();
        }
        const auto result = save_write_policy(directory, settings);
        admission.lock();
        if (result == FQDB_OK || result == FQDB_COMMIT_UNKNOWN) {
            std::lock_guard<std::mutex> lock(periodic_mutex);
            write_policy = settings;
            write_policy_saved = result == FQDB_OK;
            write_policy.struct_size = sizeof(write_policy);
            write_epoch = std::chrono::steady_clock::now();
            periodic_changed.notify_one();
        }
        require(result == FQDB_OK, result);
    } catch (...) {
        if (!admission.owns_lock())
            admission.lock();
        reconfiguring = false;
        admission_changed.notify_all();
        throw;
    }
    reconfiguring = false;
    admission_changed.notify_all();
}
void Core::periodic_loop() noexcept {
    std::vector<CommitWaiter *> batch;
    AdaptiveCommitWait waiting(AdaptiveCommitWait::parallel_execution());
    std::unique_lock<std::mutex> lock(periodic_mutex);
    auto flush = [&] {
        lock.unlock();
        try {
            std::lock_guard<WriterGate> guard(writer);
            sync_locked();
        } catch (...) {
            poisoned.store(true, std::memory_order_release);
            fqdb_status expected = FQDB_OK;
            write_failure.compare_exchange_strong(expected, current_error(),
                                                  std::memory_order_release);
        }
        lock.lock();
    };
    for (;;) {
#ifdef FQDB_TESTING
        periodic_iterations.fetch_add(1, std::memory_order_relaxed);
#endif
        auto now = std::chrono::steady_clock::now();
        auto period = std::chrono::milliseconds(write_policy.period_ms);
        auto elapsed = now - write_epoch;
        auto deadline = now + (period - elapsed % period);
        const bool scheduled = options.durability == FQDB_DURABILITY_SYNC &&
                               write_failure.load(std::memory_order_acquire) == FQDB_OK;
        if (scheduled && dirty_bytes.load(std::memory_order_relaxed) &&
            (periodic_stop ||
             dirty_bytes.load(std::memory_order_relaxed) >= write_policy.threshold_bytes ||
             now >= write_epoch + period)) {
            flush();
            write_epoch = std::chrono::steady_clock::now();
            continue;
        }
        if (!periodic_head) {
            if (periodic_stop)
                return;
            const auto spins = waiting.begin_idle(now);
            if (spins && dirty_bytes.load(std::memory_order_relaxed) &&
                write_policy.mode == FQDB_WRITE_PERFORMANCE && CommitSignal::native()) {
                auto until = now + AdaptiveCommitWait::maximum_window;
                if (scheduled)
                    until = std::min(until, write_epoch + period);
                lock.unlock();
                AdaptiveCommitWait::spin(periodic_ready, spins, until);
                lock.lock();
                // Recheck stop/configuration and the original flush triggers.
                continue;
            }
            if (scheduled && dirty_bytes.load(std::memory_order_relaxed))
                periodic_changed.wait_until(lock, deadline, [&] {
                    return periodic_stop || periodic_head ||
                           dirty_bytes.load(std::memory_order_relaxed) >=
                               write_policy.threshold_bytes;
                });
            else {
                // An empty database has no periodic deadline or disk work.
#ifdef FQDB_TESTING
                periodic_idle_waits.fetch_add(1, std::memory_order_relaxed);
#endif
                periodic_changed.wait(lock, [&] {
                    return periodic_stop || periodic_head ||
                           (options.durability == FQDB_DURABILITY_SYNC &&
                            write_failure.load(std::memory_order_acquire) == FQDB_OK &&
                            dirty_bytes.load(std::memory_order_relaxed));
                });
            }
            continue;
        }
        waiting.arrived(now);
        batch.clear();
        try {
            batch.reserve(std::min<size_t>(periodic_count, 256));
        } catch (...) {
            auto r = periodic_head;
            periodic_head = r->next;
            if (!periodic_head)
                periodic_tail = nullptr;
            --periodic_count;
            periodic_ready.store(bool(periodic_head), std::memory_order_relaxed);
            periodic_bytes -= r->queued_bytes;
            periodic_pending_bytes -= r->queued_bytes;
            r->status = FQDB_NO_MEMORY;
            r->release_private();
            periodic_completed = r->queue_sequence;
            complete_periodic(*r);
            periodic_drained.notify_all();
            continue;
        }
        const bool reliable = periodic_head->reliable;
        while (periodic_head && periodic_head->reliable == reliable &&
               batch.size() < group_max.load(std::memory_order_relaxed)) {
            auto r = periodic_head;
            periodic_head = r->next;
            if (!periodic_head)
                periodic_tail = nullptr;
            --periodic_count;
            periodic_bytes -= r->queued_bytes;
            batch.push_back(r);
        }
        periodic_ready.store(bool(periodic_head), std::memory_order_relaxed);
        lock.unlock();
        size_t next = 0;
        while (next < batch.size()) {
            const auto begin = next;
            process_group(
                batch.data() + begin, batch.size() - begin,
                [&]() { return next < batch.size() ? batch[next++] : nullptr; }, reliable);
            lock.lock();
            for (size_t i = begin; i < next; ++i) {
                auto r = batch[i];
                periodic_pending_bytes -= r->queued_bytes;
                periodic_completed = r->queue_sequence;
                complete_periodic(*r);
            }
            waiting.completed(!reliable && write_policy.mode == FQDB_WRITE_PERFORMANCE);
            periodic_drained.notify_all();
            // A detached batch can contain many complete execution groups.
            // Check the flush trigger between them rather than accumulating
            // an entire admission window beyond the configured byte target.
            if (!reliable && options.durability == FQDB_DURABILITY_SYNC &&
                write_failure.load(std::memory_order_acquire) == FQDB_OK &&
                dirty_bytes.load(std::memory_order_relaxed) &&
                (dirty_bytes.load(std::memory_order_relaxed) >= write_policy.threshold_bytes ||
                 std::chrono::steady_clock::now() >=
                     write_epoch + std::chrono::milliseconds(write_policy.period_ms))) {
                flush();
                write_epoch = std::chrono::steady_clock::now();
            }
            lock.unlock();
        }
        lock.lock();
    }
}

#pragma once
#include "select_execute.hpp"

namespace fengqudb::paged {
inline std::shared_ptr<const select::Program>
select_program(Transaction &tx, std::string_view request,
               std::chrono::steady_clock::time_point started, bool &compiled) {
    require(request.size() <= 262144, FQDB_LIMIT);
    auto &core = *tx.base->core;
    auto expected =
        std::atomic_load_explicit(&core.select_program_cache, std::memory_order_acquire);
    if (expected && expected->request == request)
        return expected;
    // Conservative reservation covers decoded AST, literal strings, compiled
    // patterns and bounded planner temporaries before allocating any of them.
    uint64_t reservation = 262144 + request.size() * 192;
    Charge allocation;
    bool cache = false;
    if (core.options.cache_bytes && reservation <= 1048576 &&
        core.options.pinned_bytes - core.pin_budget->used() >=
            reservation + FQDB_MAX_VALUE_BYTES + 262144) {
        try {
            allocation = Charge(core.pin_budget, {}, reservation);
            cache = true;
        } catch (const Error &e) {
            if (e.status != FQDB_LIMIT)
                throw;
        }
    }
    if (!cache)
        allocation = Charge(tx.budget, core.transaction_budget, reservation);
    compiled = true;
    auto program =
        std::make_shared<select::Program>(request, std::move(allocation), reservation, tx, started);
    require(reservation <= program->spec.memory_bytes, FQDB_LIMIT);
    std::shared_ptr<const select::Program> result = program;
    if (cache && program->reservation <= 1048576)
        std::atomic_compare_exchange_strong_explicit(&core.select_program_cache, &expected, result,
                                                     std::memory_order_acq_rel,
                                                     std::memory_order_acquire);
    return result;
}
inline FeatureResult select_query(Transaction &tx, const FeatureRequest &request,
                                  Charge &retained) {
    tx.ready();
    auto started = std::chrono::steady_clock::now();
    require(request.scope.empty() && request.key.empty() && !request.argument && request.score == 0,
            FQDB_INVALID_ARGUMENT);
    require(request.value.size() <= 262144, FQDB_LIMIT);
    Bytes semantic;
    semantic.reserve(request.value.size());
    std::string_view after;
    bool seen_after = false;
    wire::Reader input(request.value);
    wire::Field field;
    for (;;) {
        auto start = input.position();
        if (!input.next(field))
            break;
        if (field.number == 18) {
            require(!seen_after, FQDB_INVALID_ARGUMENT);
            seen_after = true;
            after = select::str(input, field);
        } else {
            require(input.skip(field), FQDB_INVALID_ARGUMENT);
            semantic.append(request.value.substr(start, input.position() - start));
        }
    }
    require(input.done(), FQDB_INVALID_ARGUMENT);
    bool compiled = false;
    auto program = select_program(tx, semantic, started, compiled);
    select::Memory memory(tx, program->spec.memory_bytes, program->reservation);
    memory.peak = std::max(memory.peak, program->compiled_memory->peak);
    select::Executor executor(tx, *program, memory, request.operation == FQDB_SELECT_EXPLAIN, after,
                              &program, started, compiled ? program->compile_steps : 0);
    executor.work.deadline = started + std::chrono::milliseconds(program->spec.timeout_ms);
    executor.work.check();
    auto bytes = executor.encode(request.operation == FQDB_SELECT_EXPLAIN);
    require(bytes.size() <= FQDB_MAX_TRANSACTION_BYTES, FQDB_LIMIT);
    retained = Charge(tx.budget, tx.base->core->transaction_budget, bytes.capacity() + 128);
    FeatureResult result;
    result.value = std::move(bytes);
    return result;
}
} // namespace fengqudb::paged

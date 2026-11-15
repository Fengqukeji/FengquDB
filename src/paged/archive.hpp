#pragma once
#include "maintenance.hpp"
#include "features.hpp"

namespace fengqudb::paged {
inline void publish_output(OutputDirectory &output) {
    auto status = output.publish();
    require(status == FQDB_OK, status);
}
inline void copy_directory(Core &source, const Capture &base, OutputDirectory &output,
                           MaintenanceJob &job) {
    auto uuid = new_uuid();
    auto filename = generation_name(uuid, 1);
    output.retain_file(join(output.path(), "CURRENT"));
    CompactTarget target(join(output.path(), filename), uuid, 1, source, job, base.time);
    output.retain_file(join(output.path(), filename));
    verify_index(*base.view, [&] { job.check(); });
    target.copy(*base.view);
    target.view.checkpoint.origin_hi = base.view->checkpoint.origin_hi;
    target.view.checkpoint.origin_lo = base.view->checkpoint.origin_lo;
    target.seal(base.view->version);
    require(write_current(output.path(), {uuid, 1}) == FQDB_OK, FQDB_IO_ERROR);
    target.generation->remove_on_close.store(false);
    output.retain_file(join(output.path(),"WRITE"));
    require(save_write_policy(output.path(),source.get_write_policy())==FQDB_OK,FQDB_IO_ERROR);
}
inline fqdb_store_maintenance_info
archive_increment(Core &core, const std::string &destination, uint64_t from,
                  const fqdb_store_maintenance_options &settings) {
    MaintenanceJob job(core, settings);
    auto base = job.capture();
    auto source = base->view;
    require(from >= history_floor(*source) && from <= source->version, FQDB_HISTORY_LOST);
    OutputDirectory output(destination, core.directory, source->generation->uuid);
    auto uuid = new_uuid();
    auto filename = generation_name(uuid, 1);
    output.retain_file(join(output.path(), "CURRENT"));
    fqdb_store_maintenance_info info{};
    info.snapshot_version = from;
    info.final_version = source->version;
    info.source_file_bytes = job.source_bytes();
    {
        Generation archive(0);
        archive.remove_on_close.store(true);
        archive.create(join(output.path(), filename), uuid, 1);
        output.retain_file(join(output.path(), filename));
        Checkpoint checkpoint;
        checkpoint.version = from;
        checkpoint.floor = from;
        checkpoint.features = 2;
        checkpoint.origin_hi = storage::get64(source->generation->uuid.data());
        checkpoint.origin_lo = storage::get64(source->generation->uuid.data() + 8);
        archive.seal(checkpoint);
        uint64_t expected = from;
        HistoryReader history(source, from);
        while (auto record = history.next([&] { job.check(); })) {
            const auto &frame = record->frame;
            Input header(*record->generation->file, frame.payload, frame.payload + frame.length);
            require(header.tag() == 17);
            auto timestamp = header.fixed(8);
            uint64_t features = 0;
            // First pass only validates metadata/flags. Values are fetched in
            // the second pass, one source transaction at a time.
            read_wal(*record->generation, frame, [](const WalOperation &) {}, &features);
            WalBatch batch(frame.version, timestamp, features);
            uint64_t retained = 0;
            Charge charge(core.transaction_budget, {}, 0);
            read_wal(*record->generation, frame, [&](const WalOperation &operation) {
                job.check();
                Value value;
                if (!operation.result.deleted()) {
                    retained += operation.result.size + 128;
                    charge.resize(retained);
                    value = record->generation->value(operation.result, frame.end, false, false);
                }
                Ref previous;
                previous.version = operation.base.version;
                batch.add(value ? OpKind::put : OpKind::erase, operation.key, value, previous,
                          operation.result.expires);
                ++info.copied_keys;
            });
            batch.append(archive);
            expected = frame.version;
        }
        require(expected == source->version, FQDB_HISTORY_LOST);
        checkpoint.version = expected;
        checkpoint.time = std::max(base->time, source->checkpoint.time);
        checkpoint.features = 2 | source->checkpoint.features;
        archive.seal(checkpoint);
        job.check();
        require(write_current(output.path(), {uuid, 1}) == FQDB_OK, FQDB_IO_ERROR);
        archive.remove_on_close.store(false);
        info.output_file_bytes = archive.tail;
    }
    job.check();
    maintenance_at(60);
    publish_output(output);
    maintenance_at(61);
    return info;
}
inline fqdb_store_maintenance_info recover_chain(const std::string &base_path,
                                                 const std::vector<std::string> &archives,
                                                 const std::string &destination,
                                                 uint64_t target_version, uint64_t target_time,
                                                 const fqdb_store_maintenance_options &settings) {
    auto opts = defaults();
    opts.flags = 0;
    auto source = std::make_shared<Core>(base_path, opts);
    source->open();
    MaintenanceJob job(*source, settings);
    auto base = job.capture();
    require(!target_version || !target_time, FQDB_INVALID_ARGUMENT);
    require(!target_version || target_version >= base->view->version, FQDB_HISTORY_LOST);
    require(!target_time || target_time >= base->view->checkpoint.time, FQDB_HISTORY_LOST);
    auto origin_hi = base->view->checkpoint.origin_hi;
    auto origin_lo = base->view->checkpoint.origin_lo;
    if (!origin_hi && !origin_lo) {
        origin_hi = storage::get64(base->view->generation->uuid.data());
        origin_lo = storage::get64(base->view->generation->uuid.data() + 8);
    }
    struct InputArchive {
        std::shared_ptr<Generation> generation;
        Checkpoint checkpoint;
    };
    std::vector<InputArchive> inputs;
    inputs.reserve(archives.size());
    uint64_t chain_end = base->view->version, coverage_end = base->view->checkpoint.time;
    for (const auto &path : archives) {
        job.check();
        auto manifest = read_current(path);
        auto archive = std::make_shared<Generation>(0);
        archive->open(join(path, manifest.filename()), manifest.uuid, manifest.generation, true);
        auto checkpoint = archive->checkpoint();
        require((checkpoint.features & 2) && checkpoint.origin_hi == origin_hi &&
                    checkpoint.origin_lo == origin_lo && checkpoint.floor == chain_end &&
                    checkpoint.version >= checkpoint.floor,
                FQDB_HISTORY_LOST);
        chain_end = checkpoint.version;
        coverage_end = checkpoint.time;
        inputs.push_back({std::move(archive), checkpoint});
    }
    require(!target_version || target_version <= chain_end, FQDB_HISTORY_LOST);
    require(!target_time || target_time <= coverage_end, FQDB_HISTORY_LOST);
    OutputDirectory output(destination, source->directory, base->view->generation->uuid);
    copy_directory(*source, *base, output, job);
    output.release_lease();
    fqdb_store_maintenance_info info{};
    info.snapshot_version = base->view->version;
    info.source_file_bytes = job.source_bytes();
    {
        auto target_options = defaults();
        target_options.flags = 0;
        target_options.durability = FQDB_DURABILITY_BUFFERED;
        auto target = std::make_shared<Core>(output.path(), target_options);
        target->open();
        uint64_t coverage_time = base->view->checkpoint.time;
        bool stopped = target_version && target_version == target->current()->version;
        for (const auto &input : inputs) {
            if (stopped)
                break;
            job.check();
            auto &archive = *input.generation;
            auto checkpoint = input.checkpoint;
            require(checkpoint.floor == target->current()->version, FQDB_HISTORY_LOST);
            coverage_time = checkpoint.time;
            uint64_t expected = checkpoint.floor, position = data_start;
            while (position < checkpoint.end) {
                job.check();
                auto frame = archive.frame(position, checkpoint.end);
                require(bool(frame));
                position = frame->end;
                if (frame->kind != Frame::Kind::wal)
                    continue;
                require(expected != UINT64_MAX && frame->version == expected + 1);
                Input header(*archive.file, frame->payload, frame->payload + frame->length);
                require(header.tag() == 17);
                auto timestamp = header.fixed(8);
                if ((target_version && frame->version > target_version) ||
                    (target_time && timestamp > target_time)) {
                    stopped = true;
                    break;
                }
                uint64_t features = 0;
                read_wal(archive, *frame, [](const WalOperation &) {}, &features);
                Transaction tx(target, 0);
                tx.skip_indexes = true;
                tx.feature_bits = features & 4;
                tx.imported_time = timestamp;
                if (features & 1)
                    enable_features(tx);
                require((features & target->current()->checkpoint.features) ==
                        target->current()->checkpoint.features);
                read_wal(archive, *frame, [&](const WalOperation &operation) {
                    job.check();
                    if (operation.result.deleted())
                        tx.erase(operation.key);
                    else {
                        auto value = archive.value(operation.result, frame->end, false, false);
                        tx.put(operation.key, *value, operation.result.expires);
                    }
                    ++info.copied_keys;
                });
                target->commit_checked(tx, nullptr, [&] { job.check(); });
                expected = frame->version;
                if (target_version && expected == target_version) {
                    stopped = true;
                    break;
                }
            }
            require(stopped || expected == checkpoint.version);
        }
        require(!target_version || target->current()->version == target_version, FQDB_HISTORY_LOST);
        require(!target_time || stopped || target_time <= coverage_time, FQDB_HISTORY_LOST);
        target->checkpoint();
        auto view = std::make_shared<View>(*target->current());
        view->checkpoint.origin_hi = origin_hi;
        view->checkpoint.origin_lo = origin_lo;
        view->generation->seal(view->checkpoint);
        view->limit = view->generation->tail;
        target->publish(view);
        target->verify(settings);
        info.final_version = view->version;
        info.output_file_bytes = view->limit;
        info.stored_keys = view->keys;
        info.stored_value_bytes = view->live_bytes;
    }
    job.check();
    publish_output(output);
    return info;
}
} // namespace fengqudb::paged

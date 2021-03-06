/*
 * Copyright (C) 2015 ScyllaDB
 */

/*
 * This file is part of Scylla.
 *
 * Scylla is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Scylla is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Scylla.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <vector>
#include <map>
#include <functional>
#include <utility>
#include <assert.h>
#include <algorithm>

#include <boost/range/algorithm.hpp>
#include <boost/range/adaptors.hpp>
#include <boost/range/join.hpp>
#include <boost/algorithm/cxx11/any_of.hpp>

#include <seastar/core/future-util.hh>
#include <seastar/core/scheduling.hh>

#include "sstables.hh"
#include "sstables/progress_monitor.hh"
#include "sstables/sstables_manager.hh"
#include "compaction.hh"
#include "compaction_manager.hh"
#include "database.hh"
#include "mutation_reader.hh"
#include "schema.hh"
#include "db/system_keyspace.hh"
#include "service/storage_service.hh"
#include "service/priority_manager.hh"
#include "db_clock.hh"
#include "mutation_compactor.hh"
#include "leveled_manifest.hh"
#include "utils/observable.hh"
#include "dht/token.hh"
#include "mutation_writer/shard_based_splitting_writer.hh"
#include "mutation_source_metadata.hh"

namespace sstables {

logging::logger clogger("compaction");

static api::timestamp_type get_max_purgeable_timestamp(const column_family& cf, sstable_set::incremental_selector& selector,
        const std::unordered_set<shared_sstable>& compacting_set, const dht::decorated_key& dk) {
    auto timestamp = api::max_timestamp;
    std::optional<utils::hashed_key> hk;
    for (auto&& sst : boost::range::join(selector.select(dk).sstables, cf.compacted_undeleted_sstables())) {
        if (compacting_set.count(sst)) {
            continue;
        }
        if (!hk) {
            hk = sstables::sstable::make_hashed_key(*cf.schema(), dk.key());
        }
        if (sst->filter_has_key(*hk)) {
            timestamp = std::min(timestamp, sst->get_stats_metadata().min_timestamp);
        }
    }
    return timestamp;
}

static bool belongs_to_current_node(const dht::token& t, const dht::token_range_vector& sorted_owned_ranges) {
    auto low = std::lower_bound(sorted_owned_ranges.begin(), sorted_owned_ranges.end(), t,
            [] (const range<dht::token>& a, const dht::token& b) {
        // check that range a is before token b.
        return a.after(b, dht::token_comparator());
    });

    if (low != sorted_owned_ranges.end()) {
        const dht::token_range& r = *low;
        return r.contains(t, dht::token_comparator());
    }

    return false;
}

static std::vector<shared_sstable> get_uncompacting_sstables(column_family& cf, std::vector<shared_sstable> sstables) {
    auto all_sstables = boost::copy_range<std::vector<shared_sstable>>(*cf.get_sstables_including_compacted_undeleted());
    boost::sort(all_sstables, [] (const shared_sstable& x, const shared_sstable& y) {
        return x->generation() < y->generation();
    });
    std::sort(sstables.begin(), sstables.end(), [] (const shared_sstable& x, const shared_sstable& y) {
        return x->generation() < y->generation();
    });
    std::vector<shared_sstable> not_compacted_sstables;
    boost::set_difference(all_sstables, sstables,
        std::back_inserter(not_compacted_sstables), [] (const shared_sstable& x, const shared_sstable& y) {
            return x->generation() < y->generation();
        });
    return not_compacted_sstables;
}

class compaction;

struct compaction_writer {
    sstable_writer writer;
    shared_sstable sst;
};

class compacting_sstable_writer {
    compaction& _c;
    std::optional<compaction_writer> _writer = {};
public:
    explicit compacting_sstable_writer(compaction& c) : _c(c) { }
    void consume_new_partition(const dht::decorated_key& dk);

    void consume(tombstone t) { _writer->writer.consume(t); }
    stop_iteration consume(static_row&& sr, tombstone, bool) { return _writer->writer.consume(std::move(sr)); }
    stop_iteration consume(clustering_row&& cr, row_tombstone, bool) { return _writer->writer.consume(std::move(cr)); }
    stop_iteration consume(range_tombstone&& rt) { return _writer->writer.consume(std::move(rt)); }

    stop_iteration consume_end_of_partition();
    void consume_end_of_stream();
};

struct compaction_read_monitor_generator final : public read_monitor_generator {
    class compaction_read_monitor final : public  sstables::read_monitor, public backlog_read_progress_manager {
        sstables::shared_sstable _sst;
        compaction_manager& _compaction_manager;
        column_family& _cf;
        const sstables::reader_position_tracker* _tracker = nullptr;
        uint64_t _last_position_seen = 0;
    public:
        virtual void on_read_started(const sstables::reader_position_tracker& tracker) override {
            _tracker = &tracker;
            _cf.get_compaction_strategy().get_backlog_tracker().register_compacting_sstable(_sst, *this);
        }

        virtual void on_read_completed() override {
            if (_tracker) {
                _last_position_seen = _tracker->position;
                _tracker = nullptr;
            }
        }

        virtual uint64_t compacted() const override {
            if (_tracker) {
                return _tracker->position;
            }
            return _last_position_seen;
        }

        void remove_sstable(bool is_tracking) {
            if (is_tracking && _sst) {
                _cf.get_compaction_strategy().get_backlog_tracker().remove_sstable(_sst);
            } else if (_sst) {
                _cf.get_compaction_strategy().get_backlog_tracker().revert_charges(_sst);
            }
            _sst = {};
        }

        compaction_read_monitor(sstables::shared_sstable sst, compaction_manager& cm, column_family &cf)
            : _sst(std::move(sst)), _compaction_manager(cm), _cf(cf) { }

        ~compaction_read_monitor() {
            // We failed to finish handling this SSTable, so we have to update the backlog_tracker
            // about it.
            if (_sst) {
                _cf.get_compaction_strategy().get_backlog_tracker().revert_charges(_sst);
            }
        }

        friend class compaction_read_monitor_generator;
    };

    virtual sstables::read_monitor& operator()(sstables::shared_sstable sst) override {
        _generated_monitors.emplace_back(std::move(sst), _compaction_manager, _cf);
        return _generated_monitors.back();
    }
    compaction_read_monitor_generator(compaction_manager& cm, column_family& cf)
        : _compaction_manager(cm)
        , _cf(cf) {}

    void remove_sstables(bool is_tracking) {
        for (auto& rm : _generated_monitors) {
            rm.remove_sstable(is_tracking);
        }
    }

    void remove_sstable(bool is_tracking, sstables::shared_sstable& sst) {
        for (auto& rm : _generated_monitors) {
            if (rm._sst == sst) {
                rm.remove_sstable(is_tracking);
                break;
            }
        }
    }
private:
     compaction_manager& _compaction_manager;
     column_family& _cf;
     std::deque<compaction_read_monitor> _generated_monitors;
};

class compaction_write_monitor final : public sstables::write_monitor, public backlog_write_progress_manager {
    sstables::shared_sstable _sst;
    column_family& _cf;
    const sstables::writer_offset_tracker* _tracker = nullptr;
    uint64_t _progress_seen = 0;
    api::timestamp_type _maximum_timestamp;
    unsigned _sstable_level;
public:
    compaction_write_monitor(sstables::shared_sstable sst, column_family& cf, api::timestamp_type max_timestamp, unsigned sstable_level)
        : _sst(sst)
        , _cf(cf)
        , _maximum_timestamp(max_timestamp)
        , _sstable_level(sstable_level)
    {}

    ~compaction_write_monitor() {
        if (_sst) {
            _cf.get_compaction_strategy().get_backlog_tracker().revert_charges(_sst);
        }
    }

    virtual void on_write_started(const sstables::writer_offset_tracker& tracker) override {
        _tracker = &tracker;
        _cf.get_compaction_strategy().get_backlog_tracker().register_partially_written_sstable(_sst, *this);
    }

    virtual void on_data_write_completed() override {
        if (_tracker) {
            _progress_seen = _tracker->offset;
            _tracker = nullptr;
        }
    }

    virtual uint64_t written() const {
        if (_tracker) {
            return _tracker->offset;
        }
        return _progress_seen;
    }

    void add_sstable() {
        _cf.get_compaction_strategy().get_backlog_tracker().add_sstable(_sst);
        _sst = {};
    }

    api::timestamp_type maximum_timestamp() const override {
        return _maximum_timestamp;
    }

    unsigned level() const override {
        return _sstable_level;
    }

    virtual void on_write_completed() override { }
    virtual void on_flush_completed() override { }
};

// Writes a temporary sstable run containing only garbage collected data.
// Whenever regular compaction writer seals a new sstable, this writer will flush a new sstable as well,
// right before there's an attempt to release exhausted sstables earlier.
// Generated sstables will be temporarily added to table to make sure that a compaction crash will not
// result in data resurrection.
// When compaction finishes, all the temporary sstables generated here will be deleted and removed
// from table's sstable set.
class garbage_collected_sstable_writer {
    compaction* _c = nullptr;
    std::vector<shared_sstable> _temp_sealed_gc_sstables;
    std::deque<compaction_write_monitor> _active_write_monitors = {};
    shared_sstable _sst;
    std::optional<sstable_writer> _writer;
    std::optional<utils::observer<>> _on_new_sstable_sealed_observer;
    utils::UUID _run_identifier = utils::make_random_uuid();
    bool _consuming_new_partition {};
private:
    void setup_on_new_sstable_sealed_handler();
    void maybe_create_new_sstable_writer();
    void finish_sstable_writer();
    void on_end_of_stream();
public:
    garbage_collected_sstable_writer() = default;
    explicit garbage_collected_sstable_writer(compaction& c) : _c(&c) {
        setup_on_new_sstable_sealed_handler();
    }

    garbage_collected_sstable_writer& operator=(const garbage_collected_sstable_writer&) = delete;
    garbage_collected_sstable_writer(const garbage_collected_sstable_writer&) = delete;

    garbage_collected_sstable_writer(garbage_collected_sstable_writer&& other)
            : _c(other._c)
            , _temp_sealed_gc_sstables(std::move(other._temp_sealed_gc_sstables))
            , _active_write_monitors(std::move(other._active_write_monitors))
            , _sst(std::move(other._sst))
            , _writer(std::move(other._writer))
            , _run_identifier(other._run_identifier)
            , _consuming_new_partition(other._consuming_new_partition) {
        other._on_new_sstable_sealed_observer->disconnect();
        setup_on_new_sstable_sealed_handler();
    }

    garbage_collected_sstable_writer& operator=(garbage_collected_sstable_writer&& other) {
        if (this != &other) {
            this->~garbage_collected_sstable_writer();
            new (this) garbage_collected_sstable_writer(std::move(other));
        }
        return *this;
    }

    void consume_new_partition(const dht::decorated_key& dk) {
        maybe_create_new_sstable_writer();
        _writer->consume_new_partition(dk);
        _consuming_new_partition = true;
    }

    void consume(tombstone t) { _writer->consume(t); }
    stop_iteration consume(static_row&& sr, tombstone, bool) { return _writer->consume(std::move(sr)); }
    stop_iteration consume(clustering_row&& cr, row_tombstone, bool) { return _writer->consume(std::move(cr)); }
    stop_iteration consume(range_tombstone&& rt) { return _writer->consume(std::move(rt)); }

    stop_iteration consume_end_of_partition() {
        _writer->consume_end_of_partition();
        _consuming_new_partition = false;
        return stop_iteration::no;
    }

    void consume_end_of_stream() {
        finish_sstable_writer();
        on_end_of_stream();
    }
};

// Resharding doesn't really belong into any strategy, because it is not worried about laying out
// SSTables according to any strategy-specific criteria.  So we will just make it proportional to
// the amount of data we still have to reshard.
//
// Although at first it may seem like we could improve this by tracking the ongoing reshard as well
// and reducing the backlog as we compact, that is not really true. Resharding is not really
// expected to get rid of data and it is usually just splitting data among shards. Whichever backlog
// we get rid of by tracking the compaction will come back as a big spike as we add this SSTable
// back to their rightful shard owners.
//
// So because the data is supposed to be constant, we will just add the total amount of data as the
// backlog.
class resharding_backlog_tracker final : public compaction_backlog_tracker::impl {
    uint64_t _total_bytes = 0;
public:
    virtual double backlog(const compaction_backlog_tracker::ongoing_writes& ow, const compaction_backlog_tracker::ongoing_compactions& oc) const override {
        return _total_bytes;
    }

    virtual void add_sstable(sstables::shared_sstable sst)  override {
        _total_bytes += sst->data_size();
    }

    virtual void remove_sstable(sstables::shared_sstable sst)  override {
        _total_bytes -= sst->data_size();
    }
};

class compaction {
protected:
    column_family& _cf;
    creator_fn _sstable_creator;
    schema_ptr _schema;
    std::vector<shared_sstable> _sstables;
    // Unused sstables are tracked because if compaction is interrupted we can only delete them.
    // Deleting used sstables could potentially result in data loss.
    std::vector<shared_sstable> _new_unused_sstables;
    lw_shared_ptr<sstable_set> _compacting;
    uint64_t _max_sstable_size;
    uint32_t _sstable_level;
    lw_shared_ptr<compaction_info> _info = make_lw_shared<compaction_info>();
    uint64_t _estimated_partitions = 0;
    std::vector<unsigned long> _ancestors;
    db::replay_position _rp;
    encoding_stats_collector _stats_collector;
    utils::observable<> _on_new_sstable_sealed;
    bool _contains_multi_fragment_runs = false;
    mutation_source_metadata _ms_metadata = {};
protected:
    compaction(column_family& cf, creator_fn creator, std::vector<shared_sstable> sstables, uint64_t max_sstable_size, uint32_t sstable_level)
        : _cf(cf)
        , _sstable_creator(std::move(creator))
        , _schema(cf.schema())
        , _sstables(std::move(sstables))
        , _max_sstable_size(max_sstable_size)
        , _sstable_level(sstable_level)
    {
        _info->cf = &cf;
        for (auto& sst : _sstables) {
            _stats_collector.update(sst->get_encoding_stats_for_compaction());
        }
        std::unordered_set<utils::UUID> ssts_run_ids;
        _contains_multi_fragment_runs = std::any_of(_sstables.begin(), _sstables.end(), [&ssts_run_ids] (shared_sstable& sst) {
            return !ssts_run_ids.insert(sst->run_identifier()).second;
        });
        _cf.get_compaction_manager().register_compaction(_info);
    }

    uint64_t partitions_per_sstable() const {
        uint64_t estimated_sstables = std::max(1UL, uint64_t(ceil(double(_info->start_size) / _max_sstable_size)));
        return std::min(uint64_t(ceil(double(_estimated_partitions) / estimated_sstables)),
                        _cf.get_compaction_strategy().adjust_partition_estimate(_ms_metadata, _estimated_partitions));
    }

    void setup_new_sstable(shared_sstable& sst) {
        _info->new_sstables.push_back(sst);
        _new_unused_sstables.push_back(sst);
        sst->get_metadata_collector().set_replay_position(_rp);
        sst->get_metadata_collector().sstable_level(_sstable_level);
        for (auto ancestor : _ancestors) {
            sst->add_ancestor(ancestor);
        }
    }

    void finish_new_sstable(compaction_writer* writer) {
        writer->writer.consume_end_of_stream();
        writer->sst->open_data().get0();
        _info->end_size += writer->sst->bytes_on_disk();
        // Notify GC'ed-data sstable writer's handler that an output sstable has just been sealed.
        // The handler is responsible for making sure that deleting an input sstable will not
        // result in resurrection on failure.
        _on_new_sstable_sealed();
    }

    api::timestamp_type maximum_timestamp() const {
        auto m = std::max_element(_sstables.begin(), _sstables.end(), [] (const shared_sstable& sst1, const shared_sstable& sst2) {
            return sst1->get_stats_metadata().max_timestamp < sst2->get_stats_metadata().max_timestamp;
        });
        return (*m)->get_stats_metadata().max_timestamp;
    }

    utils::observer<> add_on_new_sstable_sealed_handler(std::function<void (void)> handler) noexcept {
        return _on_new_sstable_sealed.observe(std::move(handler));
    }

    encoding_stats get_encoding_stats() const {
        return _stats_collector.get();
    }

    virtual compaction_completion_desc
    get_compaction_completion_desc(std::vector<shared_sstable> input_sstables, std::vector<shared_sstable> output_sstables) {
        return compaction_completion_desc{std::move(input_sstables), std::move(output_sstables)};
    }
public:
    compaction& operator=(const compaction&) = delete;
    compaction(const compaction&) = delete;

    virtual ~compaction() {
        if (_info) {
            _cf.get_compaction_manager().deregister_compaction(_info);
        }
    }
private:
    // Default range sstable reader that will only return mutation that belongs to current shard.
    virtual flat_mutation_reader make_sstable_reader() const = 0;

    template <typename GCConsumer>
    GCC6_CONCEPT(
        requires CompactedFragmentsConsumer<GCConsumer>
    )
    future<> setup(GCConsumer gc_consumer) {
        auto ssts = make_lw_shared<sstables::sstable_set>(_cf.get_compaction_strategy().make_sstable_set(_schema));
        sstring formatted_msg = "[";
        auto fully_expired = get_fully_expired_sstables(_cf, _sstables, gc_clock::now() - _schema->gc_grace_seconds());

        for (auto& sst : _sstables) {
            // Compacted sstable keeps track of its ancestors.
            _ancestors.push_back(sst->generation());
            _info->start_size += sst->bytes_on_disk();
            _info->total_partitions += sst->get_estimated_key_count();
            formatted_msg += format("{}:level={:d}, ", sst->get_filename(), sst->get_sstable_level());

            // Do not actually compact a sstable that is fully expired and can be safely
            // dropped without ressurrecting old data.
            if (fully_expired.count(sst)) {
                continue;
            }

            // We also capture the sstable, so we keep it alive while the read isn't done
            ssts->insert(sst);
            // FIXME: If the sstables have cardinality estimation bitmaps, use that
            // for a better estimate for the number of partitions in the merged
            // sstable than just adding up the lengths of individual sstables.
            _estimated_partitions += sst->get_estimated_key_count();
            // TODO:
            // Note that this is not fully correct. Since we might be merging sstables that originated on
            // another shard (#cpu changed), we might be comparing RP:s with differing shard ids,
            // which might vary in "comparable" size quite a bit. However, since the worst that happens
            // is that we might miss a high water mark for the commit log replayer,
            // this is kind of ok, esp. since we will hopefully not be trying to recover based on
            // compacted sstables anyway (CL should be clean by then).
            _rp = std::max(_rp, sst->get_stats_metadata().position);
        }
        formatted_msg += "]";
        _info->sstables = _sstables.size();
        _info->ks_name = _schema->ks_name();
        _info->cf_name = _schema->cf_name();
        report_start(formatted_msg);

        _compacting = std::move(ssts);

        auto now = gc_clock::now();
        auto consumer = make_interposer_consumer([this, gc_consumer = std::move(gc_consumer), now] (flat_mutation_reader reader) mutable
        {
            using compact_mutations = compact_for_compaction<compacting_sstable_writer, GCConsumer>;
            auto cfc = make_stable_flattened_mutations_consumer<compact_mutations>(*schema(), now,
                                         max_purgeable_func(),
                                         get_compacting_sstable_writer(),
                                         std::move(gc_consumer));

            return seastar::async([cfc = std::move(cfc), reader = std::move(reader), this] () mutable {
                reader.consume_in_thread(std::move(cfc), make_partition_filter(), db::no_timeout);
            });
        });
        return consumer(make_sstable_reader());
    }

    virtual reader_consumer make_interposer_consumer(reader_consumer end_consumer) = 0;

    compaction_info finish(std::chrono::time_point<db_clock> started_at, std::chrono::time_point<db_clock> ended_at) {
        _info->ended_at = std::chrono::duration_cast<std::chrono::milliseconds>(ended_at.time_since_epoch()).count();
        auto ratio = double(_info->end_size) / double(_info->start_size);
        auto duration = std::chrono::duration<float>(ended_at - started_at);
        // Don't report NaN or negative number.
        auto throughput = duration.count() > 0 ? (double(_info->end_size) / (1024*1024)) / duration.count() : double{};
        sstring new_sstables_msg;

        on_end_of_compaction();

        for (auto& newtab : _info->new_sstables) {
            new_sstables_msg += format("{}:level={:d}, ", newtab->get_filename(), newtab->get_sstable_level());
        }

        // FIXME: there is some missing information in the log message below.
        // look at CompactionTask::runMayThrow() in origin for reference.
        // - add support to merge summary (message: Partition merge counts were {%s}.).
        // - there is no easy way, currently, to know the exact number of total partitions.
        // By the time being, using estimated key count.
        sstring formatted_msg = sprint("%ld sstables to [%s]. %ld bytes to %ld (~%d%% of original) in %dms = %.2fMB/s. " \
            "~%ld total partitions merged to %ld.",
            _info->sstables, new_sstables_msg, _info->start_size, _info->end_size, int(ratio * 100),
            std::chrono::duration_cast<std::chrono::milliseconds>(duration).count(), throughput,
            _info->total_partitions, _info->total_keys_written);
        report_finish(formatted_msg, ended_at);

        backlog_tracker_adjust_charges();

        auto info = std::move(_info);
        _cf.get_compaction_manager().deregister_compaction(info);
        return std::move(*info);
    }

    virtual void report_start(const sstring& formatted_msg) const = 0;
    virtual void report_finish(const sstring& formatted_msg, std::chrono::time_point<db_clock> ended_at) const = 0;
    virtual void backlog_tracker_adjust_charges() = 0;

    virtual std::function<api::timestamp_type(const dht::decorated_key&)> max_purgeable_func() {
        return [] (const dht::decorated_key& dk) {
            return api::min_timestamp;
        };
    }

    virtual flat_mutation_reader::filter make_partition_filter() const {
        return [] (const dht::decorated_key&) {
            return true;
        };
    }

    virtual void on_new_partition() = 0;

    virtual void on_end_of_compaction() = 0;

    virtual shared_sstable create_new_sstable() const = 0;

    // create a writer based on decorated key.
    virtual compaction_writer create_compaction_writer(const dht::decorated_key& dk) = 0;
    // stop current writer
    virtual void stop_sstable_writer(compaction_writer* writer) = 0;

    compacting_sstable_writer get_compacting_sstable_writer() {
        return compacting_sstable_writer(*this);
    }

    const schema_ptr& schema() const {
        return _schema;
    }

    void delete_sstables_for_interrupted_compaction() {
        // Delete either partially or fully written sstables of a compaction that
        // was either stopped abruptly (e.g. out of disk space) or deliberately
        // (e.g. nodetool stop COMPACTION).
        for (auto& sst : _new_unused_sstables) {
            clogger.debug("Deleting sstable {} of interrupted compaction for {}.{}", sst->get_filename(), _info->ks_name, _info->cf_name);
            sst->mark_for_deletion();
        }
    }

    void setup_garbage_collected_sstable(shared_sstable sst) {
        // Add new sstable to table's set because expired tombstone should be available if compaction is abruptly stopped.
        _cf.add_sstable_and_update_cache(std::move(sst)).get();
    }

    void eventually_delete_garbage_collected_sstable(shared_sstable sst) {
        // Add sstable to compaction's input list for it to be eventually removed from table's set.
        sst->mark_for_deletion();
        _sstables.push_back(std::move(sst));
    }
public:
    garbage_collected_sstable_writer make_garbage_collected_sstable_writer() {
        return garbage_collected_sstable_writer(*this);
    }

    bool contains_multi_fragment_runs() const {
        return _contains_multi_fragment_runs;
    }

    template <typename GCConsumer = noop_compacted_fragments_consumer>
    GCC6_CONCEPT(
        requires CompactedFragmentsConsumer<GCConsumer>
    )
    static future<compaction_info> run(std::unique_ptr<compaction> c, GCConsumer gc_consumer = GCConsumer());

    friend class compacting_sstable_writer;
    friend class garbage_collected_sstable_writer;
};

void compacting_sstable_writer::consume_new_partition(const dht::decorated_key& dk) {
    if (_c._info->is_stop_requested()) {
        // Compaction manager will catch this exception and re-schedule the compaction.
        throw compaction_stop_exception(_c._info->ks_name, _c._info->cf_name, _c._info->stop_requested);
    }
    if (!_writer) {
        _writer = _c.create_compaction_writer(dk);
    }

    _c.on_new_partition();
    _writer->writer.consume_new_partition(dk);
    _c._info->total_keys_written++;
}

stop_iteration compacting_sstable_writer::consume_end_of_partition() {
    auto ret = _writer->writer.consume_end_of_partition();
    if (ret == stop_iteration::yes) {
        // stop sstable writer being currently used.
        _c.stop_sstable_writer(&*_writer);
        _writer = std::nullopt;
    }
    return ret;
}

void compacting_sstable_writer::consume_end_of_stream() {
    if (_writer) {
        _c.stop_sstable_writer(&*_writer);
        _writer = std::nullopt;
    }
}

void garbage_collected_sstable_writer::setup_on_new_sstable_sealed_handler() {
    _on_new_sstable_sealed_observer = _c->add_on_new_sstable_sealed_handler([this] {
        // NOTE: This handler is called, BEFORE an input sstable is possibly deleted
        // *AND* AFTER a new output sstable is sealed, to flush a garbage collected
        // sstable being currently written.
        // That way, data is resurrection is prevented by making sure that the
        // GC'able data is still reachable in a temporary sstable.
        assert(!_consuming_new_partition);
        // Wait for current gc'ed-only-sstable to be flushed and added to table's set.
        this->finish_sstable_writer();
    });
}

void garbage_collected_sstable_writer::maybe_create_new_sstable_writer() {
    if (!_writer) {
        _sst = _c->create_new_sstable();

        auto&& priority = service::get_local_compaction_priority();
        _active_write_monitors.emplace_back(_sst, _c->_cf, _c->maximum_timestamp(), _c->_sstable_level);
        sstable_writer_config cfg = _c->_cf.get_sstables_manager().configure_writer();
        cfg.run_identifier = _run_identifier;
        cfg.monitor = &_active_write_monitors.back();
        _writer.emplace(_sst->get_writer(*_c->schema(), _c->partitions_per_sstable(), cfg, _c->get_encoding_stats(), priority));
    }
}

void garbage_collected_sstable_writer::finish_sstable_writer() {
    if (_writer) {
        _writer->consume_end_of_stream();
        _writer = std::nullopt;
        _sst->open_data().get0();
        _c->setup_garbage_collected_sstable(_sst);
        _temp_sealed_gc_sstables.push_back(std::move(_sst));
    }
}

void garbage_collected_sstable_writer::on_end_of_stream() {
    for (auto&& sst : _temp_sealed_gc_sstables) {
        clogger.debug("Asking for deletion of temporary tombstone-only sstable {}", sst->get_filename());
        _c->eventually_delete_garbage_collected_sstable(std::move(sst));
    }
}

class regular_compaction : public compaction {
    replacer_fn _replacer;
    std::unordered_set<shared_sstable> _compacting_for_max_purgeable_func;
    // store a clone of sstable set for column family, which needs to be alive for incremental selector.
    sstable_set _set;
    // used to incrementally calculate max purgeable timestamp, as we iterate through decorated keys.
    std::optional<sstable_set::incremental_selector> _selector;
    // sstable being currently written.
    std::optional<compaction_weight_registration> _weight_registration;
    mutable compaction_read_monitor_generator _monitor_generator;
    std::deque<compaction_write_monitor> _active_write_monitors = {};
    utils::UUID _run_identifier;
public:
    regular_compaction(column_family& cf, compaction_descriptor descriptor)
        : compaction(cf, std::move(descriptor.creator), std::move(descriptor.sstables), descriptor.max_sstable_bytes, descriptor.level)
        , _replacer(std::move(descriptor.replacer))
        , _compacting_for_max_purgeable_func(std::unordered_set<shared_sstable>(_sstables.begin(), _sstables.end()))
        , _set(cf.get_sstable_set())
        , _selector(_set.make_incremental_selector())
        , _weight_registration(std::move(descriptor.weight_registration))
        , _monitor_generator(_cf.get_compaction_manager(), _cf)
        , _run_identifier(descriptor.run_identifier)
    {
        _info->run_identifier = _run_identifier;
    }

    flat_mutation_reader make_sstable_reader() const override {
        return ::make_local_shard_sstable_reader(_schema,
                no_reader_permit(),
                _compacting,
                query::full_partition_range,
                _schema->full_slice(),
                service::get_local_compaction_priority(),
                tracing::trace_state_ptr(),
                ::streamed_mutation::forwarding::no,
                ::mutation_reader::forwarding::no,
                _monitor_generator);
    }

    reader_consumer make_interposer_consumer(reader_consumer end_consumer) override {
        return _cf.get_compaction_strategy().make_interposer_consumer(_ms_metadata, std::move(end_consumer));
    }

    void report_start(const sstring& formatted_msg) const override {
        clogger.info("Compacting {}", formatted_msg);
    }

    void report_finish(const sstring& formatted_msg, std::chrono::time_point<db_clock> ended_at) const override {
        clogger.info("Compacted {}", formatted_msg);
    }

    void backlog_tracker_adjust_charges() override {
        _monitor_generator.remove_sstables(_info->tracking);
        for (auto& wm : _active_write_monitors) {
            wm.add_sstable();
        }
    }

    virtual std::function<api::timestamp_type(const dht::decorated_key&)> max_purgeable_func() override {
        return [this] (const dht::decorated_key& dk) {
            return get_max_purgeable_timestamp(_cf, *_selector, _compacting_for_max_purgeable_func, dk);
        };
    }

    virtual flat_mutation_reader::filter make_partition_filter() const override {
        return [&s = *_schema] (const dht::decorated_key& dk){
            return dht::shard_of(s, dk.token()) == this_shard_id();
        };
    }

    virtual shared_sstable create_new_sstable() const override {
        return _sstable_creator(this_shard_id());
    }

    virtual compaction_writer create_compaction_writer(const dht::decorated_key& dk) override {
        auto sst = _sstable_creator(this_shard_id());
        setup_new_sstable(sst);

        _active_write_monitors.emplace_back(sst, _cf, maximum_timestamp(), _sstable_level);
        auto&& priority = service::get_local_compaction_priority();
        sstable_writer_config cfg = _cf.get_sstables_manager().configure_writer();
        cfg.max_sstable_size = _max_sstable_size;
        cfg.monitor = &_active_write_monitors.back();
        cfg.run_identifier = _run_identifier;
        return compaction_writer{sst->get_writer(*_schema, partitions_per_sstable(), cfg, get_encoding_stats(), priority), sst};
    }

    virtual void stop_sstable_writer(compaction_writer* writer) override {
        if (writer) {
            finish_new_sstable(writer);
            maybe_replace_exhausted_sstables_by_sst(writer->sst);
        }
    }

    void on_new_partition() override {
        update_pending_ranges();
    }

    virtual void on_end_of_compaction() override {
        if (_weight_registration) {
            _cf.get_compaction_manager().on_compaction_complete(*_weight_registration);
        }
        replace_remaining_exhausted_sstables();
    }
private:
    void backlog_tracker_incrementally_adjust_charges(std::vector<shared_sstable> exhausted_sstables) {
        //
        // Notify backlog tracker of an early sstable replacement triggered by incremental compaction approach.
        // Backlog tracker will be told that the exhausted sstables aren't being compacted anymore, and the
        // new sstables, which replaced the exhausted ones, are not partially written sstables and they can
        // be added to tracker like any other regular sstable in the table's set.
        // This way we prevent bogus calculation of backlog due to lack of charge adjustment whenever there's
        // an early sstable replacement.
        //

        for (auto& sst : exhausted_sstables) {
            _monitor_generator.remove_sstable(_info->tracking, sst);
        }
        for (auto& wm : _active_write_monitors) {
            wm.add_sstable();
        }
        _active_write_monitors.clear();
    }

    void maybe_replace_exhausted_sstables_by_sst(shared_sstable sst) {
        // Skip earlier replacement of exhausted sstables if compaction works with only single-fragment runs,
        // meaning incremental compaction is disabled for this compaction.
        if (!_contains_multi_fragment_runs) {
            return;
        }
        // Replace exhausted sstable(s), if any, by new one(s) in the column family.
        auto not_exhausted = [s = _schema, &dk = sst->get_last_decorated_key()] (shared_sstable& sst) {
            return sst->get_last_decorated_key().tri_compare(*s, dk) > 0;
        };
        auto exhausted = std::partition(_sstables.begin(), _sstables.end(), not_exhausted);

        if (exhausted != _sstables.end()) {
            // The goal is that exhausted sstables will be deleted as soon as possible,
            // so we need to release reference to them.
            std::for_each(exhausted, _sstables.end(), [this] (shared_sstable& sst) {
                _compacting_for_max_purgeable_func.erase(sst);
                // Fully expired sstable is not actually compacted, therefore it's not present in the compacting set.
                _compacting->erase(sst);
            });
            auto exhausted_ssts = std::vector<shared_sstable>(exhausted, _sstables.end());
            _replacer(get_compaction_completion_desc(exhausted_ssts, std::move(_new_unused_sstables)));
            _sstables.erase(exhausted, _sstables.end());
            backlog_tracker_incrementally_adjust_charges(std::move(exhausted_ssts));
        }
    }

    void replace_remaining_exhausted_sstables() {
        if (!_sstables.empty()) {
            std::vector<shared_sstable> sstables_compacted;
            std::move(_sstables.begin(), _sstables.end(), std::back_inserter(sstables_compacted));
            _replacer(get_compaction_completion_desc(std::move(sstables_compacted), std::move(_new_unused_sstables)));
        }
    }

    void update_pending_ranges() {
        if (_set.all()->empty() || _info->pending_replacements.empty()) { // set can be empty for testing scenario.
            return;
        }
        auto set = _set;
        // Releases reference to sstables compacted by this compaction or another, both of which belongs
        // to the same column family
        for (auto& pending_replacement : _info->pending_replacements) {
            for (auto& sst : pending_replacement.removed) {
                // Set may not contain sstable to be removed because this compaction may have started
                // before the creation of that sstable.
                if (!set.all()->count(sst)) {
                    continue;
                }
                set.erase(sst);
            }
            for (auto& sst : pending_replacement.added) {
                set.insert(sst);
            }
        }
        _set = std::move(set);
        _selector.emplace(_set.make_incremental_selector());
        _info->pending_replacements.clear();
    }
};

class cleanup_compaction final : public regular_compaction {
    dht::token_range_vector _owned_ranges;
private:
    dht::partition_range_vector
    get_ranges_for_invalidation(const std::vector<shared_sstable>& sstables) {
        auto owned_ranges = dht::to_partition_ranges(_owned_ranges);

        auto non_owned_ranges = boost::copy_range<dht::partition_range_vector>(sstables
                | boost::adaptors::transformed([] (const shared_sstable& sst) {
            return dht::partition_range::make({sst->get_first_decorated_key(), true},
                                              {sst->get_last_decorated_key(), true});
        }));
        // optimize set of potentially overlapping ranges by deoverlapping them.
        non_owned_ranges = dht::partition_range::deoverlap(std::move(non_owned_ranges), dht::ring_position_comparator(*_schema));

        // subtract *each* owned range from the partition range of *each* sstable*,
        // such that we'll be left only with a set of non-owned ranges.
        for (auto& owned_range : owned_ranges) {
            dht::partition_range_vector new_non_owned_ranges;
            for (auto& non_owned_range : non_owned_ranges) {
                auto ret = non_owned_range.subtract(owned_range, dht::ring_position_comparator(*_schema));
                new_non_owned_ranges.insert(new_non_owned_ranges.end(), ret.begin(), ret.end());
            }
            non_owned_ranges = std::move(new_non_owned_ranges);
        }
        return non_owned_ranges;
    }
protected:
    virtual compaction_completion_desc
    get_compaction_completion_desc(std::vector<shared_sstable> input_sstables, std::vector<shared_sstable> output_sstables) override {
        auto ranges_for_for_invalidation = get_ranges_for_invalidation(input_sstables);
        return compaction_completion_desc{std::move(input_sstables), std::move(output_sstables), std::move(ranges_for_for_invalidation)};
    }
public:
    cleanup_compaction(column_family& cf, compaction_descriptor descriptor)
        : regular_compaction(cf, std::move(descriptor))
        , _owned_ranges(service::get_local_storage_service().get_local_ranges(_schema->ks_name()))
    {
        _info->type = compaction_type::Cleanup;
    }

    void report_start(const sstring& formatted_msg) const override {
        clogger.info("Cleaning {}", formatted_msg);
    }

    void report_finish(const sstring& formatted_msg, std::chrono::time_point<db_clock> ended_at) const override {
        clogger.info("Cleaned {}", formatted_msg);
    }

    flat_mutation_reader::filter make_partition_filter() const override {
        return [this] (const dht::decorated_key& dk) {
            if (dht::shard_of(*_schema, dk.token()) != this_shard_id()) {
                clogger.trace("Token {} does not belong to CPU {}, skipping", dk.token(), this_shard_id());
                return false;
            }

            if (!belongs_to_current_node(dk.token(), _owned_ranges)) {
                clogger.trace("Token {} does not belong to this node, skipping", dk.token());
                return false;
            }
            return true;
        };
    }
};

class scrub_compaction final : public regular_compaction {
    class reader : public flat_mutation_reader::impl {
        bool _skip_corrupted;
        flat_mutation_reader _reader;
        mutation_fragment_stream_validator _validator;
        bool _skip_to_next_partition = false;

    private:
        void maybe_abort_scrub() {
            if (!_skip_corrupted) {
                throw compaction_stop_exception(_schema->ks_name(), _schema->cf_name(), "scrub compaction found invalid data", false);
            }
        }

        void on_unexpected_partition_start(const mutation_fragment& ps) {
            maybe_abort_scrub();
            const auto& new_key = ps.as_partition_start().key();
            const auto& current_key = _validator.previous_partition_key();
            clogger.error("[scrub compaction {}.{}] Unexpected partition-start for partition {} ({}),"
                    " rectifying by adding assumed missing partition-end to the current partition {} ({}).",
                    _schema->ks_name(),
                    _schema->cf_name(),
                    new_key.key().with_schema(*_schema),
                    new_key,
                    current_key.key().with_schema(*_schema),
                    current_key);

            auto pe = mutation_fragment(partition_end{});
            if (!_validator(pe)) {
                throw compaction_stop_exception(
                        _schema->ks_name(),
                        _schema->cf_name(),
                        "scrub compaction failed to rectify unexpected partition-start, validator rejects the injected partition-end",
                        false);
            }
            push_mutation_fragment(std::move(pe));

            if (!_validator(ps)) {
                throw compaction_stop_exception(
                        _schema->ks_name(),
                        _schema->cf_name(),
                        "scrub compaction failed to rectify unexpected partition-start, validator rejects it even after the injected partition-end",
                        false);
            }
        }

        void on_invalid_partition(const dht::decorated_key& new_key) {
            maybe_abort_scrub();
            const auto& current_key = _validator.previous_partition_key();
            clogger.error("[scrub compaction {}.{}] Skipping invalid partition {} ({}):"
                    " partition has non-monotonic key compared to current one {} ({})",
                    _schema->ks_name(),
                    _schema->cf_name(),
                    new_key.key().with_schema(*_schema),
                    new_key,
                    current_key.key().with_schema(*_schema),
                    current_key);
            _skip_to_next_partition = true;
        }

        void on_invalid_mutation_fragment(const mutation_fragment& mf) {
            maybe_abort_scrub();
            const auto& key = _validator.previous_partition_key();
            clogger.error("[scrub compaction {}.{}] Skipping invalid {} fragment {}in partition {} ({}):"
                    " fragment has non-monotonic position {} compared to previous position {}.",
                    _schema->ks_name(),
                    _schema->cf_name(),
                    mf.mutation_fragment_kind(),
                    mf.has_key() ? format("with key {} ({}) ", mf.key().with_schema(*_schema), mf.key()) : "",
                    key.key().with_schema(*_schema),
                    key,
                    mf.position(),
                    _validator.previous_position());
        }

        void on_invalid_end_of_stream() {
            maybe_abort_scrub();
            // Handle missing partition_end
            push_mutation_fragment(partition_end{});
            clogger.error("[scrub compaction {}.{}] Adding missing partition-end to the end of the stream.",
                    _schema->ks_name(), _schema->cf_name());
        }

        void fill_buffer_from_underlying() {
            while (!_reader.is_buffer_empty() && !is_buffer_full()) {
                auto mf = _reader.pop_mutation_fragment();
                if (mf.is_partition_start()) {
                    // First check that fragment kind monotonicity stands.
                    // When skipping to another partition the fragment
                    // monotonicity of the partition-start doesn't have to be
                    // and shouldn't be verified. We know the last fragment the
                    // validator saw is a partition-start, passing it another one
                    // will confuse it.
                    if (!_skip_to_next_partition && !_validator(mf)) {
                        on_unexpected_partition_start(mf);
                        // Continue processing this partition start.
                    }
                    _skip_to_next_partition = false;
                    // Then check that the partition monotonicity stands.
                    const auto& dk = mf.as_partition_start().key();
                    if (!_validator(dk)) {
                        on_invalid_partition(dk);
                        continue;
                    }
                } else if (_skip_to_next_partition) {
                    continue;
                } else {
                    if (!_validator(mf)) {
                        on_invalid_mutation_fragment(mf);
                        continue;
                    }
                }
                push_mutation_fragment(std::move(mf));
            }

            _end_of_stream = _reader.is_end_of_stream() && _reader.is_buffer_empty();

            if (_end_of_stream) {
                if (!_validator.on_end_of_stream()) {
                    on_invalid_end_of_stream();
                }
            }
        }

    public:
        reader(flat_mutation_reader underlying, bool skip_corrupted)
            : impl(underlying.schema())
            , _skip_corrupted(skip_corrupted)
            , _reader(std::move(underlying))
            , _validator(*_schema) {
        }
        virtual future<> fill_buffer(db::timeout_clock::time_point timeout) override {
            return repeat([this, timeout] {
                return _reader.fill_buffer(timeout).then([this] {
                    fill_buffer_from_underlying();
                    return stop_iteration(is_buffer_full() || _end_of_stream);
                });
            }).handle_exception([this] (std::exception_ptr e) {
                try {
                    std::rethrow_exception(std::move(e));
                } catch (const compaction_stop_exception&) {
                    // Propagate these unchanged.
                    throw;
                } catch (const storage_io_error&) {
                    // Propagate these unchanged.
                    throw;
                } catch (...) {
                    // We don't want failed scrubs to be retried.
                    throw compaction_stop_exception(
                            _schema->ks_name(),
                            _schema->cf_name(),
                            format("scrub compaction failed due to unrecoverable error: {}", std::current_exception()),
                            false);
                }
            });
        }
        virtual void next_partition() override {
            throw_with_backtrace<std::bad_function_call>();
        }
        virtual future<> fast_forward_to(const dht::partition_range& pr, db::timeout_clock::time_point timeout) override {
            return make_exception_future<>(make_backtraced_exception_ptr<std::bad_function_call>());
        }
        virtual future<> fast_forward_to(position_range pr, db::timeout_clock::time_point timeout) override {
            return make_exception_future<>(make_backtraced_exception_ptr<std::bad_function_call>());
        }
        virtual size_t buffer_size() const override {
            return flat_mutation_reader::impl::buffer_size() + _reader.buffer_size();
        }
    };

private:
    compaction_options::scrub _options;

public:
    scrub_compaction(column_family& cf, compaction_descriptor descriptor, compaction_options::scrub options)
        : regular_compaction(cf, std::move(descriptor))
        , _options(options) {
        _info->type = compaction_type::Scrub;
    }

    void report_start(const sstring& formatted_msg) const override {
        clogger.info("Scrubbing {}", formatted_msg);
    }

    void report_finish(const sstring& formatted_msg, std::chrono::time_point<db_clock> ended_at) const override {
        clogger.info("Finished scrubbing {}", formatted_msg);
    }

    flat_mutation_reader make_sstable_reader() const override {
        return make_flat_mutation_reader<reader>(regular_compaction::make_sstable_reader(), _options.skip_corrupted);
    }

    friend flat_mutation_reader make_scrubbing_reader(flat_mutation_reader rd, bool skip_corrupted);
};

flat_mutation_reader make_scrubbing_reader(flat_mutation_reader rd, bool skip_corrupted) {
    return make_flat_mutation_reader<scrub_compaction::reader>(std::move(rd), skip_corrupted);
}

class resharding_compaction final : public compaction {
    std::vector<std::pair<shared_sstable, std::optional<sstable_writer>>> _output_sstables;
    shard_id _shard; // shard of current sstable writer
    compaction_backlog_tracker _resharding_backlog_tracker;

    // Partition count estimation for a shard S:
    //
    // TE, the total estimated partition count for a shard S, is defined as
    // TE = Sum(i = 0...N) { Ei / Si }.
    //
    // where i is an input sstable that belongs to shard S,
    //       Ei is the estimated partition count for sstable i,
    //       Si is the total number of shards that own sstable i.
    //
    struct estimated_values {
        uint64_t estimated_size = 0;
        uint64_t estimated_partitions = 0;
    };
    std::vector<estimated_values> _estimation_per_shard;
    std::vector<utils::UUID> _run_identifiers;
private:
    // return estimated partitions per sstable for a given shard
    uint64_t partitions_per_sstable(shard_id s) const {
        uint64_t estimated_sstables = std::max(uint64_t(1), uint64_t(ceil(double(_estimation_per_shard[s].estimated_size) / _max_sstable_size)));
        // As we adjust this estimate downwards from the compaction strategy, it can get to 0 so
        // make sure we're returning at least 1.
        return std::max(uint64_t(1),
                std::min(uint64_t(ceil(double(_estimation_per_shard[s].estimated_partitions) / estimated_sstables)),
                _cf.get_compaction_strategy().adjust_partition_estimate(_ms_metadata, _estimation_per_shard[s].estimated_partitions)));
    }
public:
    resharding_compaction(column_family& cf, sstables::compaction_descriptor descriptor)
        : compaction(cf, std::move(descriptor.creator), std::move(descriptor.sstables), descriptor.max_sstable_bytes, descriptor.level)
        , _output_sstables(smp::count)
        , _resharding_backlog_tracker(std::make_unique<resharding_backlog_tracker>())
        , _estimation_per_shard(smp::count)
        , _run_identifiers(smp::count)
    {
        cf.get_compaction_manager().register_backlog_tracker(_resharding_backlog_tracker);
        for (auto& sst : _sstables) {
            _resharding_backlog_tracker.add_sstable(sst);

            const auto& shards = sst->get_shards_for_this_sstable();
            auto size = sst->bytes_on_disk();
            auto estimated_partitions = sst->get_estimated_key_count();
            for (auto& s : shards) {
                _estimation_per_shard[s].estimated_size += std::max(uint64_t(1), uint64_t(ceil(double(size) / shards.size())));
                _estimation_per_shard[s].estimated_partitions += std::max(uint64_t(1), uint64_t(ceil(double(estimated_partitions) / shards.size())));
            }
        }
        for (auto i : boost::irange(0u, smp::count)) {
            _run_identifiers[i] = utils::make_random_uuid();
        }
        _info->type = compaction_type::Reshard;
    }

    ~resharding_compaction() {
        for (auto& s : _sstables) {
            _resharding_backlog_tracker.remove_sstable(s);
        }
    }

    // Use reader that makes sure no non-local mutation will not be filtered out.
    flat_mutation_reader make_sstable_reader() const override {
        return ::make_range_sstable_reader(_schema,
                no_reader_permit(),
                _compacting,
                query::full_partition_range,
                _schema->full_slice(),
                service::get_local_compaction_priority(),
                nullptr,
                ::streamed_mutation::forwarding::no,
                ::mutation_reader::forwarding::no);

    }

    reader_consumer make_interposer_consumer(reader_consumer end_consumer) override {
        return [this, end_consumer = std::move(end_consumer)] (flat_mutation_reader reader) mutable -> future<> {
            return mutation_writer::segregate_by_shard(std::move(reader), std::move(end_consumer));
        };
    }

    void report_start(const sstring& formatted_msg) const override {
        clogger.info("Resharding {}", formatted_msg);
    }

    void report_finish(const sstring& formatted_msg, std::chrono::time_point<db_clock> ended_at) const override {
        clogger.info("Resharded {}", formatted_msg);
    }

    void backlog_tracker_adjust_charges() override { }

    shared_sstable create_new_sstable() const override {
        // create_new_sstables is used only from the garbage_collected writer.
        // It it not supposed to work with resharding compactions
        abort();
    }

    compaction_writer create_compaction_writer(const dht::decorated_key& dk) override {
        auto shard = dht::shard_of(*_schema, dk.token());
        auto sst = _sstable_creator(shard);
        setup_new_sstable(sst);

        sstable_writer_config cfg = _cf.get_sstables_manager().configure_writer();
        cfg.max_sstable_size = _max_sstable_size;
        // sstables generated for a given shard will share the same run identifier.
        cfg.run_identifier = _run_identifiers.at(shard);
        auto&& priority = service::get_local_compaction_priority();
        return compaction_writer{sst->get_writer(*_schema, partitions_per_sstable(shard), cfg, get_encoding_stats(), priority, shard), sst};
    }

    void on_new_partition() override {}

    virtual void on_end_of_compaction() override {}

    void stop_sstable_writer(compaction_writer* writer) override {
        if (writer) {
            finish_new_sstable(writer);
        }
    }
};

template <typename GCConsumer>
GCC6_CONCEPT(
    requires CompactedFragmentsConsumer<GCConsumer>
)
future<compaction_info> compaction::run(std::unique_ptr<compaction> c, GCConsumer gc_consumer) {
    return seastar::async([c = std::move(c), gc_consumer = std::move(gc_consumer)] () mutable {
        auto consumer = c->setup(std::move(gc_consumer));
        auto start_time = db_clock::now();
        try {
           consumer.get();
        } catch (...) {
            c->delete_sstables_for_interrupted_compaction();
            c = nullptr; // make sure writers are stopped while running in thread context. This is because of calls to file.close().get();
            throw;
        }

        return c->finish(std::move(start_time), db_clock::now());
    });
}

compaction_type compaction_options::type() const {
    // Maps options_variant indexes to the corresponding compaction_type member.
    static const compaction_type index_to_type[] = {compaction_type::Compaction, compaction_type::Cleanup, compaction_type::Upgrade, compaction_type::Scrub, compaction_type::Reshard};
    return index_to_type[_options.index()];
}

static std::unique_ptr<compaction> make_compaction(column_family& cf, sstables::compaction_descriptor descriptor) {
    struct {
        column_family& cf;
        sstables::compaction_descriptor&& descriptor;

        std::unique_ptr<compaction> operator()(compaction_options::reshard) {
            return std::make_unique<resharding_compaction>(cf, std::move(descriptor));
        }
        std::unique_ptr<compaction> operator()(compaction_options::regular) {
            return std::make_unique<regular_compaction>(cf, std::move(descriptor));
        }
        std::unique_ptr<compaction> operator()(compaction_options::cleanup) {
            return std::make_unique<cleanup_compaction>(cf, std::move(descriptor));
        }
        std::unique_ptr<compaction> operator()(compaction_options::upgrade) {
            return std::make_unique<cleanup_compaction>(cf, std::move(descriptor));
        }
        std::unique_ptr<compaction> operator()(compaction_options::scrub scrub_options) {
            return std::make_unique<scrub_compaction>(cf, std::move(descriptor), scrub_options);
        }
    } visitor_factory{cf, std::move(descriptor)};

    return descriptor.options.visit(visitor_factory);
}

future<compaction_info>
compact_sstables(sstables::compaction_descriptor descriptor, column_family& cf) {
    if (descriptor.sstables.empty()) {
        throw std::runtime_error(format("Called {} compaction with empty set on behalf of {}.{}", compaction_name(descriptor.options.type()),
                cf.schema()->ks_name(), cf.schema()->cf_name()));
    }
    auto c = make_compaction(cf, std::move(descriptor));
    if (c->contains_multi_fragment_runs()) {
        auto gc_writer = c->make_garbage_collected_sstable_writer();
        return compaction::run(std::move(c), std::move(gc_writer));
    }
    return compaction::run(std::move(c));
}

std::unordered_set<sstables::shared_sstable>
get_fully_expired_sstables(column_family& cf, const std::vector<sstables::shared_sstable>& compacting, gc_clock::time_point gc_before) {
    clogger.debug("Checking droppable sstables in {}.{}", cf.schema()->ks_name(), cf.schema()->cf_name());

    if (compacting.empty()) {
        return {};
    }

    std::unordered_set<sstables::shared_sstable> candidates;
    auto uncompacting_sstables = get_uncompacting_sstables(cf, compacting);
    // Get list of uncompacting sstables that overlap the ones being compacted.
    std::vector<sstables::shared_sstable> overlapping = leveled_manifest::overlapping(*cf.schema(), compacting, uncompacting_sstables);
    int64_t min_timestamp = std::numeric_limits<int64_t>::max();

    for (auto& sstable : overlapping) {
        if (sstable->get_max_local_deletion_time() >= gc_before) {
            min_timestamp = std::min(min_timestamp, sstable->get_stats_metadata().min_timestamp);
        }
    }

    auto compacted_undeleted_gens = boost::copy_range<std::unordered_set<int64_t>>(cf.compacted_undeleted_sstables()
        | boost::adaptors::transformed(std::mem_fn(&sstables::sstable::generation)));
    auto has_undeleted_ancestor = [&compacted_undeleted_gens] (auto& candidate) {
        // Get ancestors from metadata collector which is empty after restart. It works for this purpose because
        // we only need to check that a sstable compacted *in this instance* hasn't an ancestor undeleted.
        // Not getting it from sstable metadata because mc format hasn't it available.
        return boost::algorithm::any_of(candidate->get_metadata_collector().ancestors(), [&compacted_undeleted_gens] (auto gen) {
            return compacted_undeleted_gens.count(gen);
        });
    };

    // SStables that do not contain live data is added to list of possibly expired sstables.
    for (auto& candidate : compacting) {
        clogger.debug("Checking if candidate of generation {} and max_deletion_time {} is expired, gc_before is {}",
                    candidate->generation(), candidate->get_stats_metadata().max_local_deletion_time, gc_before);
        // A fully expired sstable which has an ancestor undeleted shouldn't be compacted because
        // expired data won't be purged because undeleted sstables are taken into account when
        // calculating max purgeable timestamp, and not doing it could lead to a compaction loop.
        if (candidate->get_max_local_deletion_time() < gc_before && !has_undeleted_ancestor(candidate)) {
            clogger.debug("Adding candidate of generation {} to list of possibly expired sstables", candidate->generation());
            candidates.insert(candidate);
        } else {
            min_timestamp = std::min(min_timestamp, candidate->get_stats_metadata().min_timestamp);
        }
    }

    auto it = candidates.begin();
    while (it != candidates.end()) {
        auto& candidate = *it;
        // Remove from list any candidate that may contain a tombstone that covers older data.
        if (candidate->get_stats_metadata().max_timestamp >= min_timestamp) {
            it = candidates.erase(it);
        } else {
            clogger.debug("Dropping expired SSTable {} (maxLocalDeletionTime={}, gcBefore={})",
                    candidate->get_filename(), candidate->get_stats_metadata().max_local_deletion_time, gc_before);
            it++;
        }
    }
    return candidates;
}

}

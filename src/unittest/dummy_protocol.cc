#include "unittest/dummy_protocol.hpp"

#include "errors.hpp"
#include <boost/scoped_ptr.hpp>

#include "arch/timing.hpp"
#include "concurrency/rwi_lock.hpp"
#include "concurrency/signal.hpp"
#include "concurrency/wait_any.hpp"

namespace unittest {

dummy_protocol_t::region_t dummy_protocol_t::region_t::empty() THROWS_NOTHING {
    return region_t();
}

dummy_protocol_t::region_t dummy_protocol_t::read_t::get_region() const {
    return keys;
}

dummy_protocol_t::read_t dummy_protocol_t::read_t::shard(region_t region) const {
    rassert(region_is_superset(get_region(), region));
    read_t r;
    r.keys = region_intersection(region, keys);
    return r;
}

dummy_protocol_t::read_response_t dummy_protocol_t::read_t::unshard(std::vector<read_response_t> resps, UNUSED temporary_cache_t *cache) const {
    rassert(cache != NULL);
    read_response_t combined;
    for (int i = 0; i < (int)resps.size(); i++) {
        for (std::map<std::string, std::string>::const_iterator it = resps[i].values.begin();
                it != resps[i].values.end(); it++) {
            rassert(keys.keys.count((*it).first) != 0);
            rassert(combined.values.count((*it).first) == 0);
            combined.values[(*it).first] = (*it).second;
        }
    }
    return combined;
}

dummy_protocol_t::region_t dummy_protocol_t::write_t::get_region() const {
    region_t region;
    for (std::map<std::string, std::string>::const_iterator it = values.begin();
            it != values.end(); it++) {
        region.keys.insert((*it).first);
    }
    return region;
}

dummy_protocol_t::write_t dummy_protocol_t::write_t::shard(region_t region) const {
    rassert(region_is_superset(get_region(), region));
    write_t w;
    for (std::map<std::string, std::string>::const_iterator it = values.begin();
            it != values.end(); it++) {
        if (region.keys.count((*it).first) != 0) {
            w.values[(*it).first] = (*it).second;
        }
    }
    return w;
}

dummy_protocol_t::write_response_t dummy_protocol_t::write_t::unshard(std::vector<write_response_t> resps, UNUSED temporary_cache_t *cache) const {
    rassert(cache != NULL);
    write_response_t combined;
    for (int i = 0; i < (int)resps.size(); i++) {
        for (std::map<std::string, std::string>::const_iterator it = resps[i].old_values.begin();
                it != resps[i].old_values.end(); it++) {
            rassert(values.find((*it).first) != values.end());
            rassert(combined.old_values.count((*it).first) == 0);
            combined.old_values[(*it).first] = (*it).second;
        }
    }
    return combined;
}

bool region_is_superset(dummy_protocol_t::region_t a, dummy_protocol_t::region_t b) {
    for (std::set<std::string>::const_iterator it = b.keys.begin(); it != b.keys.end(); it++) {
        if (a.keys.count(*it) == 0) {
            return false;
        }
    }
    return true;
}

dummy_protocol_t::region_t region_intersection(dummy_protocol_t::region_t a, dummy_protocol_t::region_t b) {
    dummy_protocol_t::region_t i;
    for (std::set<std::string>::const_iterator it = a.keys.begin(); it != a.keys.end(); it++) {
        if (b.keys.count(*it) != 0) {
            i.keys.insert(*it);
        }
    }
    return i;
}

dummy_protocol_t::region_t region_join(const std::vector<dummy_protocol_t::region_t> &vec) THROWS_ONLY(bad_join_exc_t, bad_region_exc_t) {
    dummy_protocol_t::region_t u;
    for (std::vector<dummy_protocol_t::region_t>::const_iterator it = vec.begin(); it != vec.end(); it++) {
        for (std::set<std::string>::iterator it2 = (*it).keys.begin(); it2 != (*it).keys.end(); it2++) {
            if (u.keys.count(*it2) != 0) throw bad_join_exc_t();
            u.keys.insert(*it2);
        }
    }
    return u;
}

std::vector<dummy_protocol_t::region_t> region_subtract_many(const dummy_protocol_t::region_t &a, const std::vector<dummy_protocol_t::region_t>& b) {
    std::vector<dummy_protocol_t::region_t> result(1, a);

    for (size_t i = 0; i < b.size(); i++) {
        for (std::set<std::string>::const_iterator j = b[i].keys.begin(); j != b[i].keys.end(); ++j) {
            result[0].keys.erase(*j);
        }
    }
    return result;
}

bool operator==(dummy_protocol_t::region_t a, dummy_protocol_t::region_t b) {
    return a.keys == b.keys;
}

bool operator!=(dummy_protocol_t::region_t a, dummy_protocol_t::region_t b) {
    return !(a == b);
}

dummy_underlying_store_t::dummy_underlying_store_t(dummy_protocol_t::region_t r) : region(r), metainfo(r, binary_blob_t()) {
    for (std::set<std::string>::iterator it = region.keys.begin();
            it != region.keys.end(); it++) {
        values[*it] = "";
        timestamps[*it] = state_timestamp_t::zero();
    }
}

dummy_store_view_t::dummy_store_view_t(dummy_underlying_store_t *p, dummy_protocol_t::region_t region) :
    store_view_t<dummy_protocol_t>(region), parent(p) { }

void dummy_store_view_t::new_read_token(boost::scoped_ptr<fifo_enforcer_sink_t::exit_read_t> &token_out) THROWS_NOTHING {
    fifo_enforcer_read_token_t token = parent->token_source.enter_read();
    token_out.reset(new fifo_enforcer_sink_t::exit_read_t(&parent->token_sink, token));
}

void dummy_store_view_t::new_write_token(boost::scoped_ptr<fifo_enforcer_sink_t::exit_write_t> &token_out) THROWS_NOTHING {
    fifo_enforcer_write_token_t token = parent->token_source.enter_write();
    token_out.reset(new fifo_enforcer_sink_t::exit_write_t(&parent->token_sink, token));
}

dummy_store_view_t::metainfo_t dummy_store_view_t::get_metainfo(boost::scoped_ptr<fifo_enforcer_sink_t::exit_read_t> &token, signal_t *interruptor) THROWS_ONLY(interrupted_exc_t) {
    boost::scoped_ptr<fifo_enforcer_sink_t::exit_read_t> local_token;
    local_token.swap(token);

    wait_interruptible(local_token.get(), interruptor);

    if (rng.randint(2) == 0) nap(rng.randint(10), interruptor);
    metainfo_t res = parent->metainfo.mask(get_region());
    return res;
}

void dummy_store_view_t::set_metainfo(const metainfo_t &new_metainfo, boost::scoped_ptr<fifo_enforcer_sink_t::exit_write_t> &token, signal_t *interruptor) THROWS_ONLY(interrupted_exc_t) {
    boost::scoped_ptr<fifo_enforcer_sink_t::exit_write_t> local_token;
    local_token.swap(token);

    wait_interruptible(local_token.get(), interruptor);

    if (rng.randint(2) == 0) nap(rng.randint(10), interruptor);

    parent->metainfo = parent->metainfo.update(new_metainfo);
}

dummy_protocol_t::read_response_t dummy_store_view_t::read(DEBUG_ONLY(const metainfo_t& expected_metainfo,)
        const dummy_protocol_t::read_t &read, boost::scoped_ptr<fifo_enforcer_sink_t::exit_read_t> &token, signal_t *interruptor) THROWS_ONLY(interrupted_exc_t) {
    dummy_protocol_t::read_response_t resp;
    {
        boost::scoped_ptr<fifo_enforcer_sink_t::exit_read_t> local_token;
        local_token.swap(token);

        wait_interruptible(local_token.get(), interruptor);

        // We allow expected_metainfo domain to be smaller than the metainfo domain
        rassert(expected_metainfo == parent->metainfo.mask(expected_metainfo.get_domain()));

        if (rng.randint(2) == 0) nap(rng.randint(10), interruptor);
        for (std::set<std::string>::iterator it = read.keys.keys.begin();
                it != read.keys.keys.end(); it++) {
            rassert(get_region().keys.count(*it) != 0);
            resp.values[*it] = parent->values[*it];
        }
    }
    if (rng.randint(2) == 0) nap(rng.randint(10), interruptor);
    return resp;
}

dummy_protocol_t::write_response_t dummy_store_view_t::write(DEBUG_ONLY(const metainfo_t& expected_metainfo,)
        const metainfo_t& new_metainfo, const dummy_protocol_t::write_t &write, transition_timestamp_t timestamp, boost::scoped_ptr<fifo_enforcer_sink_t::exit_write_t> &token,
        signal_t *interruptor) THROWS_ONLY(interrupted_exc_t) {
    dummy_protocol_t::write_response_t resp;
    {
        boost::scoped_ptr<fifo_enforcer_sink_t::exit_write_t> local_token;
        local_token.swap(token);

        wait_interruptible(local_token.get(), interruptor);

        // We allow expected_metainfo domain to be smaller than the metainfo domain
        rassert(expected_metainfo == parent->metainfo.mask(expected_metainfo.get_domain()));

        if (rng.randint(2) == 0) nap(rng.randint(10));
        for (std::map<std::string, std::string>::const_iterator it = write.values.begin();
                it != write.values.end(); it++) {
            resp.old_values[(*it).first] = parent->values[(*it).first];
            parent->values[(*it).first] = (*it).second;
            parent->timestamps[(*it).first] = timestamp.timestamp_after();
        }

        parent->metainfo = parent->metainfo.update(new_metainfo);
    }
    if (rng.randint(2) == 0) nap(rng.randint(10));
    return resp;
}

bool dummy_store_view_t::send_backfill(const region_map_t<dummy_protocol_t,state_timestamp_t> &start_point, const boost::function<bool(const metainfo_t&)> &should_backfill,
        const boost::function<void(dummy_protocol_t::backfill_chunk_t)> &chunk_fun, boost::scoped_ptr<fifo_enforcer_sink_t::exit_read_t> &token, signal_t *interruptor) THROWS_ONLY(interrupted_exc_t) {
    boost::scoped_ptr<fifo_enforcer_sink_t::exit_read_t> local_token;
    local_token.swap(token);

    wait_interruptible(local_token.get(), interruptor);

    metainfo_t metainfo = parent->metainfo.mask(get_region());
    if (should_backfill(metainfo)) {
        /* Make a copy so we can sleep and still have the correct semantics */
        std::map<std::string, std::string> values_snapshot = parent->values;
        std::map<std::string, state_timestamp_t> timestamps_snapshot = parent->timestamps;

        if (rng.randint(2) == 0) nap(rng.randint(10), interruptor);

        local_token.reset();

        if (rng.randint(2) == 0) nap(rng.randint(10), interruptor);
        std::vector<std::pair<dummy_protocol_t::region_t, state_timestamp_t> > pairs = start_point.get_as_pairs();
        for (int i = 0; i < (int)pairs.size(); i++) {
            for (std::set<std::string>::iterator it = pairs[i].first.keys.begin();
                    it != pairs[i].first.keys.end(); it++) {
                if (timestamps_snapshot[*it] > pairs[i].second) {
                    dummy_protocol_t::backfill_chunk_t chunk;
                    chunk.key = *it;
                    chunk.value = values_snapshot[*it];
                    chunk.timestamp = timestamps_snapshot[*it];
                    chunk_fun(chunk);
                }
                if (rng.randint(2) == 0) nap(rng.randint(10), interruptor);
            }
        }
        return true;
    } else {
        return false;
    }
}

void dummy_store_view_t::receive_backfill(const dummy_protocol_t::backfill_chunk_t &chunk, boost::scoped_ptr<fifo_enforcer_sink_t::exit_write_t> &token, signal_t *interruptor) THROWS_ONLY(interrupted_exc_t) {
    boost::scoped_ptr<fifo_enforcer_sink_t::exit_write_t> local_token;
    local_token.swap(token);

    wait_interruptible(local_token.get(), interruptor);

    rassert(get_region().keys.count(chunk.key) != 0);

    if (rng.randint(2) == 0) nap(rng.randint(10), interruptor);
    parent->values[chunk.key] = chunk.value;
    parent->timestamps[chunk.key] = chunk.timestamp;
    if (rng.randint(2) == 0) nap(rng.randint(10), interruptor);
}

void dummy_store_view_t::reset_data(dummy_protocol_t::region_t subregion, const metainfo_t &new_metainfo, boost::scoped_ptr<fifo_enforcer_sink_t::exit_write_t> &token, signal_t *interruptor) THROWS_ONLY(interrupted_exc_t) {
    boost::scoped_ptr<fifo_enforcer_sink_t::exit_write_t> local_token;
    local_token.swap(token);

    wait_interruptible(local_token.get(), interruptor);

    rassert(region_is_superset(get_region(), subregion));
    for (std::set<std::string>::iterator it = subregion.keys.begin(); it != subregion.keys.end(); it++) {
        parent->values[*it] = "";
        parent->timestamps[*it] = state_timestamp_t::zero();
    }
    parent->metainfo = parent->metainfo.update(new_metainfo);
}

}   /* namespace unittest */

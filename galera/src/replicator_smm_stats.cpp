/* Copyright (C) 2010-2017 Codership Oy <info@codersip.com> */

#include "replicator_smm.hpp"

#include <gu_debug_sync.hpp>
#include <gu_mem.h>

// @todo: should be protected static member of the parent class
static wsrep_member_status_t state2stats(galera::ReplicatorSMM::State state)
{
    switch (state)
    {
    case galera::ReplicatorSMM::S_DESTROYED :
    case galera::ReplicatorSMM::S_CLOSED    :
    case galera::ReplicatorSMM::S_CONNECTED : return WSREP_MEMBER_UNDEFINED;
    case galera::ReplicatorSMM::S_JOINING   : return WSREP_MEMBER_JOINER;
    case galera::ReplicatorSMM::S_JOINED    : return WSREP_MEMBER_JOINED;
    case galera::ReplicatorSMM::S_SYNCED    : return WSREP_MEMBER_SYNCED;
    case galera::ReplicatorSMM::S_DONOR     : return WSREP_MEMBER_DONOR;
    }

    log_fatal << "Unknown state code: " << state;
    assert(0);
    return WSREP_MEMBER_ERROR;
}

// @todo: should be protected static member of the parent class
static const char* state2stats_str(galera::ReplicatorSMM::State    state,
                                   galera::ReplicatorSMM::SstState sst_state)
{
    using galera::ReplicatorSMM;

    switch (state)
    {
    case galera::ReplicatorSMM::S_DESTROYED:
        return "Destroyed";
    case galera::ReplicatorSMM::S_CLOSED:
    case galera::ReplicatorSMM::S_CONNECTED:
    {
        if (sst_state == ReplicatorSMM::SST_REQ_FAILED)
            return "Joining: State Transfer request failed";
        else if (sst_state == ReplicatorSMM::SST_FAILED)
            return "Joining: State Transfer failed";
        else
            return "Initialized";
    }
    case galera::ReplicatorSMM::S_JOINING:
    {
        if (sst_state == ReplicatorSMM::SST_WAIT)
            return "Joining: receiving State Transfer";
        else
            return "Joining";
    }
    case galera::ReplicatorSMM::S_JOINED:
        return "Joined";
    case galera::ReplicatorSMM::S_SYNCED:
        return "Synced";
    case galera::ReplicatorSMM::S_DONOR:
        return "Donor/Desynced";
    }

    log_fatal << "Unknown state: " << state;
    assert(0);
    return "Unknown state code: ";
}

typedef enum status_vars
{
    STATS_STATE_UUID = 0,
    STATS_PROTOCOL_VERSION,
#ifdef PXC
    STATS_LAST_APPLIED,
#endif /* PXC */
    STATS_LAST_COMMITTED,
#ifdef PXC
    STATS_MONITOR_STATUS,
#endif /* PXC */
    STATS_REPLICATED,
    STATS_REPLICATED_BYTES,
    STATS_KEYS_COUNT,
    STATS_KEYS_BYTES,
    STATS_DATA_BYTES,
    STATS_UNRD_BYTES,
    STATS_RECEIVED,
    STATS_RECEIVED_BYTES,
    STATS_LOCAL_COMMITS,
    STATS_LOCAL_CERT_FAILURES,
    STATS_LOCAL_REPLAYS,
    STATS_LOCAL_SEND_QUEUE,
    STATS_LOCAL_SEND_QUEUE_MAX,
    STATS_LOCAL_SEND_QUEUE_MIN,
    STATS_LOCAL_SEND_QUEUE_AVG,
    STATS_LOCAL_RECV_QUEUE,
    STATS_LOCAL_RECV_QUEUE_MAX,
    STATS_LOCAL_RECV_QUEUE_MIN,
    STATS_LOCAL_RECV_QUEUE_AVG,
    STATS_LOCAL_CACHED_DOWNTO,
    STATS_FC_PAUSED_NS,
    STATS_FC_PAUSED_AVG,
    STATS_FC_SSENT,
//    STATS_FC_CSENT,
    STATS_FC_RECEIVED,
    STATS_FC_ACTIVE,
    STATS_FC_REQUESTED,
#ifdef PXC
    STATS_FC_INTERVAL,
    STATS_FC_INTERVAL_LOW,
    STATS_FC_INTERVAL_HIGH,
    STATS_FC_STATUS,
#endif /* PXC */
    STATS_CERT_DEPS_DISTANCE,
    STATS_APPLY_OOOE,
    STATS_APPLY_OOOL,
    STATS_APPLY_WINDOW,
    STATS_COMMIT_OOOE,
    STATS_COMMIT_OOOL,
    STATS_COMMIT_WINDOW,
    STATS_LOCAL_STATE,
    STATS_LOCAL_STATE_COMMENT,
    STATS_CERT_INDEX_SIZE,
#ifdef PXC
    STATS_CERT_BUCKET_COUNT,
    STATS_GCACHE_POOL_SIZE,
#endif /* PXC */
    STATS_CAUSAL_READS,
    STATS_CERT_INTERVAL,
    STATS_OPEN_TRX,
    STATS_OPEN_CONN,
#ifdef PXC
    STATS_IST_RECEIVE_STATUS,
    STATS_IST_RECEIVE_SEQNO_START,
    STATS_IST_RECEIVE_SEQNO_CURRENT,
    STATS_IST_RECEIVE_SEQNO_END,
#endif /* PXC */
    STATS_INCOMING_LIST,
    STATS_MAX
} StatusVars;

static const struct wsrep_stats_var wsrep_stats[STATS_MAX + 1] =
{
    { "local_state_uuid",         WSREP_VAR_STRING, { 0 }  },
    { "protocol_version",         WSREP_VAR_INT64,  { 0 }  },
#ifdef PXC
    { "last_applied",             WSREP_VAR_INT64,  { -1 } },
#endif /* PXC */
    { "last_committed",           WSREP_VAR_INT64,  { -1 } },
#ifdef PXC
    { "monitor_status (L/A/C)",   WSREP_VAR_STRING, { 0 }  },
#endif /* PXC */
    { "replicated",               WSREP_VAR_INT64,  { 0 }  },
    { "replicated_bytes",         WSREP_VAR_INT64,  { 0 }  },
    { "repl_keys",                WSREP_VAR_INT64,  { 0 }  },
    { "repl_keys_bytes",          WSREP_VAR_INT64,  { 0 }  },
    { "repl_data_bytes",          WSREP_VAR_INT64,  { 0 }  },
    { "repl_other_bytes",         WSREP_VAR_INT64,  { 0 }  },
    { "received",                 WSREP_VAR_INT64,  { 0 }  },
    { "received_bytes",           WSREP_VAR_INT64,  { 0 }  },
    { "local_commits",            WSREP_VAR_INT64,  { 0 }  },
    { "local_cert_failures",      WSREP_VAR_INT64,  { 0 }  },
    { "local_replays",            WSREP_VAR_INT64,  { 0 }  },
    { "local_send_queue",         WSREP_VAR_INT64,  { 0 }  },
    { "local_send_queue_max",     WSREP_VAR_INT64,  { 0 }  },
    { "local_send_queue_min",     WSREP_VAR_INT64,  { 0 }  },
    { "local_send_queue_avg",     WSREP_VAR_DOUBLE, { 0 }  },
    { "local_recv_queue",         WSREP_VAR_INT64,  { 0 }  },
    { "local_recv_queue_max",     WSREP_VAR_INT64,  { 0 }  },
    { "local_recv_queue_min",     WSREP_VAR_INT64,  { 0 }  },
    { "local_recv_queue_avg",     WSREP_VAR_DOUBLE, { 0 }  },
    { "local_cached_downto",      WSREP_VAR_INT64,  { 0 }  },
    { "flow_control_paused_ns",   WSREP_VAR_INT64,  { 0 }  },
    { "flow_control_paused",      WSREP_VAR_DOUBLE, { 0 }  },
    { "flow_control_sent",        WSREP_VAR_INT64,  { 0 }  },
//    { "flow_control_conts_sent",  WSREP_VAR_INT64,  { 0 }  },
    { "flow_control_recv",        WSREP_VAR_INT64,  { 0 }  },
    { "flow_control_active",      WSREP_VAR_STRING, { 0 }  },
    { "flow_control_requested",   WSREP_VAR_STRING, { 0 }  },
#ifdef PXC
    { "flow_control_interval",    WSREP_VAR_STRING, { 0 }  },
    { "flow_control_interval_low",WSREP_VAR_INT64,  { 0 }  },
    { "flow_control_interval_high",WSREP_VAR_INT64,  { 0 }, },
    { "flow_control_status",      WSREP_VAR_STRING, { 0 }  },
#endif /* PXC */
    { "cert_deps_distance",       WSREP_VAR_DOUBLE, { 0 }  },
    { "apply_oooe",               WSREP_VAR_DOUBLE, { 0 }  },
    { "apply_oool",               WSREP_VAR_DOUBLE, { 0 }  },
    { "apply_window",             WSREP_VAR_DOUBLE, { 0 }  },
    { "commit_oooe",              WSREP_VAR_DOUBLE, { 0 }  },
    { "commit_oool",              WSREP_VAR_DOUBLE, { 0 }  },
    { "commit_window",            WSREP_VAR_DOUBLE, { 0 }  },
    { "local_state",              WSREP_VAR_INT64,  { 0 }  },
    { "local_state_comment",      WSREP_VAR_STRING, { 0 }  },
    { "cert_index_size",          WSREP_VAR_INT64,  { 0 }  },
#ifdef PXC
    { "cert_bucket_count",        WSREP_VAR_INT64,  { 0 }  },
    { "gcache_pool_size",         WSREP_VAR_INT64,  { 0 }  },
#endif /* PXC */
    { "causal_reads",             WSREP_VAR_INT64,  { 0 }  },
    { "cert_interval",            WSREP_VAR_DOUBLE, { 0 }  },
    { "open_transactions",        WSREP_VAR_INT64,  { 0 }  },
    { "open_connections",         WSREP_VAR_INT64,  { 0 }  },
#ifdef PXC
    { "ist_receive_status",       WSREP_VAR_STRING, { 0 }  },
    { "ist_receive_seqno_start",  WSREP_VAR_INT64,  { 0 }  },
    { "ist_receive_seqno_current",WSREP_VAR_INT64,  { 0 }  },
    { "ist_receive_seqno_end",    WSREP_VAR_INT64,  { 0 }  },
#endif /* PXC */
    { "incoming_addresses",       WSREP_VAR_STRING, { 0 }  },
    { 0,                          WSREP_VAR_STRING, { 0 }  }
};

void
galera::ReplicatorSMM::build_stats_vars (
    std::vector<struct wsrep_stats_var>& stats)
{
    const struct wsrep_stats_var* ptr(wsrep_stats);

    do
    {
        stats.push_back(*ptr);
    }
    while (ptr++->name != 0);

    stats[STATS_STATE_UUID].value._string = state_uuid_str_;
}

#ifdef PXC
const struct wsrep_stats_var*
galera::ReplicatorSMM::stats_get()
#else
const struct wsrep_stats_var*
galera::ReplicatorSMM::stats_get() const
#endif /* PXC */
{
    if (S_DESTROYED == state_()) return 0;

    std::vector<struct wsrep_stats_var> sv(wsrep_stats_);

    sv[STATS_PROTOCOL_VERSION   ].value._int64  = protocol_version_;
#ifdef PXC
    sv[STATS_LAST_APPLIED       ].value._int64  = apply_monitor_.last_left();
    sv[STATS_LAST_COMMITTED     ].value._int64  = commit_monitor_.last_left();
#else
    wsrep_gtid last_committed;
    (void)last_committed_id(&last_committed);
    sv[STATS_LAST_COMMITTED     ].value._int64  = last_committed.seqno;
#endif /* PXC */

#ifdef PXC
    std::vector<wsrep_seqno_t> local_monitor_stats;
    local_monitor_.stats(local_monitor_stats);
    std::vector<wsrep_seqno_t> apply_monitor_stats;
    apply_monitor_.stats(apply_monitor_stats);
    std::vector<wsrep_seqno_t> commit_monitor_stats;
    commit_monitor_.stats(commit_monitor_stats);

    std::ostringstream stats_string;
    stats_string << "[ ("
               << local_monitor_stats[0] << ", "
               << local_monitor_stats[1] << "), ("
               << apply_monitor_stats[0] << ", "
               << apply_monitor_stats[1] << "), ("
               << commit_monitor_stats[0] << ", "
               << commit_monitor_stats[1] << ") ]";

    strncpy(monitor_status_string_, stats_string.str().c_str(),
            sizeof(monitor_status_string_));
    monitor_status_string_[sizeof(monitor_status_string_)-1] = 0;

    sv[STATS_MONITOR_STATUS].value._string = monitor_status_string_;
#endif /* PXC */

    sv[STATS_REPLICATED         ].value._int64  = replicated_();
    sv[STATS_REPLICATED_BYTES   ].value._int64  = replicated_bytes_();
    sv[STATS_KEYS_COUNT         ].value._int64  = keys_count_();
    sv[STATS_KEYS_BYTES         ].value._int64  = keys_bytes_();
    sv[STATS_DATA_BYTES         ].value._int64  = data_bytes_();
    sv[STATS_UNRD_BYTES         ].value._int64  = unrd_bytes_();
    sv[STATS_RECEIVED           ].value._int64  = as_->received();
    sv[STATS_RECEIVED_BYTES     ].value._int64  = as_->received_bytes();
    sv[STATS_LOCAL_COMMITS      ].value._int64  = local_commits_();
    sv[STATS_LOCAL_CERT_FAILURES].value._int64  = local_cert_failures_();
    sv[STATS_LOCAL_REPLAYS      ].value._int64  = local_replays_();

    struct gcs_stats stats;
    gcs_.get_stats (&stats);

    sv[STATS_LOCAL_SEND_QUEUE    ].value._int64  = stats.send_q_len;
    sv[STATS_LOCAL_SEND_QUEUE_MAX].value._int64  = stats.send_q_len_max;
    sv[STATS_LOCAL_SEND_QUEUE_MIN].value._int64  = stats.send_q_len_min;
    sv[STATS_LOCAL_SEND_QUEUE_AVG].value._double = stats.send_q_len_avg;
    sv[STATS_LOCAL_RECV_QUEUE    ].value._int64  = stats.recv_q_len;
    sv[STATS_LOCAL_RECV_QUEUE_MAX].value._int64  = stats.recv_q_len_max;
    sv[STATS_LOCAL_RECV_QUEUE_MIN].value._int64  = stats.recv_q_len_min;
    sv[STATS_LOCAL_RECV_QUEUE_AVG].value._double = stats.recv_q_len_avg;
#ifdef PXC
    int64_t seqno_min = gcache_.seqno_min();
    sv[STATS_LOCAL_CACHED_DOWNTO ].value._int64  =
        seqno_min != GCS_SEQNO_ILL ? seqno_min : GCS_SEQNO_NIL;
#else
    sv[STATS_LOCAL_CACHED_DOWNTO ].value._int64  = gcache_.seqno_min();
#endif /* PXC */
    sv[STATS_FC_PAUSED_NS        ].value._int64  = stats.fc_paused_ns;
    sv[STATS_FC_PAUSED_AVG       ].value._double = stats.fc_paused_avg;
    sv[STATS_FC_SSENT            ].value._int64  = stats.fc_ssent;
//    sv[STATS_FC_CSENT            ].value._int64  = stats.fc_csent;
    sv[STATS_FC_RECEIVED         ].value._int64  = stats.fc_received;
    sv[STATS_FC_ACTIVE           ].value._string = stats.fc_active    ?
        "true" : "false";
    sv[STATS_FC_REQUESTED        ].value._string = stats.fc_requested ?
        "true" : "false";

#ifdef PXC
    std::ostringstream osinterval;
    osinterval << "[ " << stats.fc_lower_limit << ", " << stats.fc_upper_limit << " ]";
    strncpy(interval_string_, osinterval.str().c_str(), sizeof(interval_string_));
    interval_string_[sizeof(interval_string_)-1] = 0;
    sv[STATS_FC_INTERVAL         ].value._string = interval_string_;
    sv[STATS_FC_INTERVAL_LOW     ].value._int64 = stats.fc_lower_limit;
    sv[STATS_FC_INTERVAL_HIGH    ].value._int64 = stats.fc_upper_limit;
    sv[STATS_FC_STATUS           ].value._string = (stats.fc_status ? "ON" : "OFF");
#endif /* PXC */

    double avg_cert_interval(0);
    double avg_deps_dist(0);
    size_t index_size(0);
    cert_.stats_get(avg_cert_interval, avg_deps_dist, index_size);

    sv[STATS_CERT_DEPS_DISTANCE  ].value._double = avg_deps_dist;
    sv[STATS_CERT_INTERVAL       ].value._double = avg_cert_interval;
    sv[STATS_CERT_INDEX_SIZE     ].value._int64  = index_size;
#ifdef PXC
    sv[STATS_CERT_BUCKET_COUNT   ].value._int64  = cert_.bucket_count();
    sv[STATS_GCACHE_POOL_SIZE    ].value._int64  = gcache_.allocated_pool_size();
#endif /* PXC */

    double oooe;
    double oool;
    double win;
    apply_monitor_.get_stats(&oooe, &oool, &win);

    sv[STATS_APPLY_OOOE          ].value._double = oooe;
    sv[STATS_APPLY_OOOL          ].value._double = oool;
    sv[STATS_APPLY_WINDOW        ].value._double = win;

    commit_monitor_.get_stats(&oooe, &oool, &win);

    sv[STATS_COMMIT_OOOE         ].value._double = oooe;
    sv[STATS_COMMIT_OOOL         ].value._double = oool;
    sv[STATS_COMMIT_WINDOW       ].value._double = win;

    if (st_.corrupt())
    {
        sv[STATS_LOCAL_STATE        ].value._int64  = WSREP_MEMBER_ERROR;
        sv[STATS_LOCAL_STATE_COMMENT].value._string = "Inconsistent";
    }
    else
    {
        sv[STATS_LOCAL_STATE        ].value._int64  =state2stats(state_());
        sv[STATS_LOCAL_STATE_COMMENT].value._string =state2stats_str(state_(),
                                                                     sst_state_);
    }
    sv[STATS_CAUSAL_READS        ].value._int64    = causal_reads_();

    Wsdb::stats wsdb_stats(wsdb_.get_stats());
    sv[STATS_OPEN_TRX].value._int64 = wsdb_stats.n_trx_;
    sv[STATS_OPEN_CONN].value._int64 = wsdb_stats.n_conn_;

#ifdef PXC
    if (ist_receiver_.running()
        && ist_receiver_.current_seqno() != WSREP_SEQNO_UNDEFINED)
    {
        // calculate %-age complete
        int percent_complete = 100;
        wsrep_seqno_t   first = ist_receiver_.first_seqno();
        wsrep_seqno_t   last = ist_receiver_.last_seqno();
        wsrep_seqno_t   current = ist_receiver_.current_seqno() - 1;

        // Now that IST processes all events to re-create cert queue current_seqno < first_seqno
        if (current >= first)
        {
          wsrep_seqno_t seq = ist_event_queue_.processed_upto();
          current = ((seq == 0) ? first : seq);
          if (last > first)
              percent_complete = 100.0 * static_cast<float>(current - first)
                                         / static_cast<float>(last - first);
          percent_complete = std::max(percent_complete, 0);
          percent_complete = std::min(percent_complete, 100);

          std::ostringstream   os;
          os << percent_complete << "% complete, received seqno "
             << current << " of " << first << "-" << last;
          strncpy(ist_status_string_, os.str().c_str(), sizeof(ist_status_string_));
          ist_status_string_[sizeof(ist_status_string_)-1] = 0;
          sv[STATS_IST_RECEIVE_STATUS].value._string = ist_status_string_;

          sv[STATS_IST_RECEIVE_SEQNO_START].value._int64 = first;
          sv[STATS_IST_RECEIVE_SEQNO_CURRENT].value._int64 = current;
          sv[STATS_IST_RECEIVE_SEQNO_END].value._int64 = last;
        }
        else
        {
          std::ostringstream   os;
          os << "preloading certification queue from "
             << current << " to " << first;
          strncpy(ist_status_string_, os.str().c_str(), sizeof(ist_status_string_));
          ist_status_string_[sizeof(ist_status_string_)-1] = 0;
          sv[STATS_IST_RECEIVE_STATUS].value._string = ist_status_string_;

          sv[STATS_IST_RECEIVE_SEQNO_START].value._int64 = first;
          sv[STATS_IST_RECEIVE_SEQNO_CURRENT].value._int64 = current;
          sv[STATS_IST_RECEIVE_SEQNO_END].value._int64 = last;
        }
    }
    else
    {
        sv[STATS_IST_RECEIVE_STATUS].value._string = "";
        sv[STATS_IST_RECEIVE_SEQNO_START].value._int64 = 0;
        sv[STATS_IST_RECEIVE_SEQNO_CURRENT].value._int64 = 0;
        sv[STATS_IST_RECEIVE_SEQNO_END].value._int64 = 0;
    }
#endif /* PXC */

    // Get gcs backend status
    gu::Status status;
    gcs_.get_status(status);
#ifdef GU_DBUG_ON
    status.insert("debug_sync_waiters", gu_debug_sync_waiters());
#endif // GU_DBUG_ON

    // Dynamical strings are copied into buffer allocated after stats var array.
    // Compute space needed.
    size_t tail_size(0);
    for (gu::Status::const_iterator i(status.begin()); i != status.end(); ++i)
    {
        tail_size += i->first.size() + 1 + i->second.size() + 1;
    }

    gu::Lock lock_inc(incoming_mutex_);
    tail_size += incoming_list_.size() + 1;

    /* Create a buffer to be passed to the caller. */
    // The buffer size needed:
    // * Space for wsrep_stats_ array
    // * Space for additional elements from status map
    // * Trailing space for string store
    size_t const vec_size(
        (sv.size() + status.size())*sizeof(struct wsrep_stats_var));
    struct wsrep_stats_var* const buf(static_cast<struct wsrep_stats_var*>(
                                      gu_malloc(vec_size + tail_size)));

    if (buf)
    {
        // Resize sv to have enough space for variables from status
        sv.resize(sv.size() + status.size());

        // Initial tail_buf position
        char* tail_buf(reinterpret_cast<char*>(buf + sv.size()));

        // Assign incoming list
        strncpy(tail_buf, incoming_list_.c_str(), incoming_list_.size() + 1);
        sv[STATS_INCOMING_LIST].value._string = tail_buf;
        tail_buf += incoming_list_.size() + 1;

        // Iterate over dynamical status variables and assing strings
        size_t sv_pos(STATS_INCOMING_LIST + 1);
        for (gu::Status::const_iterator i(status.begin());
             i != status.end(); ++i, ++sv_pos)
        {
            // Name
            strncpy(tail_buf, i->first.c_str(), i->first.size() + 1);
            sv[sv_pos].name = tail_buf;
            tail_buf += i->first.size() + 1;
            // Type
            sv[sv_pos].type = WSREP_VAR_STRING;
            // Value
            strncpy(tail_buf, i->second.c_str(), i->second.size() + 1);
            sv[sv_pos].value._string = tail_buf;
            tail_buf += i->second.size() + 1;
        }

        assert(sv_pos == sv.size() - 1);

        // NULL terminate
        sv[sv_pos].name = 0;
        sv[sv_pos].type = WSREP_VAR_STRING;
        sv[sv_pos].value._string = 0;

        assert(static_cast<size_t>
               (tail_buf - reinterpret_cast<const char*>(buf)) ==
               vec_size + tail_size);
        assert(reinterpret_cast<const char*>(buf)[vec_size + tail_size - 1] ==
               '\0');

        // Finally copy sv vector to buf
        memcpy(buf, &sv[0], vec_size);
    }
    else
    {
        log_warn << "Failed to allocate stats vars buffer to "
                 << (vec_size + tail_size)
                 << " bytes. System is running out of memory.";

    }

    return buf;
}

void
galera::ReplicatorSMM::stats_reset()
{
    if (S_DESTROYED == state_()) return;

    gcs_.flush_stats ();

    apply_monitor_.flush_stats();

    commit_monitor_.flush_stats();

    cert_.stats_reset();
}

void
galera::ReplicatorSMM::stats_free(struct wsrep_stats_var* arg)
{
    gu_free(arg);
}

#ifdef PXC
void
galera::ReplicatorSMM::fetch_pfs_info(wsrep_node_info_t* nodes, uint32_t size)
{
    gcs_.fetch_pfs_info(nodes, size);
}
#endif /* PXC */



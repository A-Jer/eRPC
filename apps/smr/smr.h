/**
 * @file smr.h
 * @brief Common code for SMR client and server
 */

#pragma once

#include <stddef.h>
extern "C" {
#include <raft/raft.h>
}

#include <libpmem.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <set>

#include "../apps_common.h"
#include "cityhash/city.h"
#include "time_entry.h"

#include "mica/table/fixedtable.h"
#include "mica/util/hash.h"

#include "util/autorun_helpers.h"

static constexpr bool kUsePmem = true;

// Key-value configuration
static constexpr size_t kAppNumKeys = MB(1);  // 1 million keys ~ ZabFPGA
static_assert(erpc::is_power_of_two(kAppNumKeys), "");

static constexpr size_t kAppKeySize = 16;
static constexpr size_t kAppValueSize = 64;
static_assert(kAppKeySize % sizeof(size_t) == 0, "");
static_assert(kAppValueSize % sizeof(size_t) == 0, "");

typedef mica::table::FixedTable<mica::table::BasicFixedTableConfig> FixedTable;
static_assert(sizeof(FixedTable::ft_key_t) == kAppKeySize, "");

// Debug/measurement
static constexpr bool kAppTimeEnt = false;
static constexpr bool kAppMeasureCommitLatency = true;  // Leader latency
static constexpr bool kAppVerbose = false;
static constexpr bool kAppEnableRaftConsoleLog = false;  // Non-null console log

// eRPC defines
static constexpr size_t kAppPhyPort = 0;
static constexpr size_t kAppNumaNode = 0;

// We run FLAGS_num_processes processes in the cluster, of which the first
// FLAGS_num_raft_servers are Raft servers, and the remaining are Raft clients.
DEFINE_uint64(num_raft_servers, 0, "Number of Raft servers");

// Return true iff this machine is a Raft server (leader or follower)
bool is_raft_server() { return FLAGS_process_id < FLAGS_num_raft_servers; }

/// The eRPC request types
enum class ReqType : uint8_t {
  kRequestVote = 3,  // Raft requestvote RPC
  kAppendEntries,    // Raft appendentries RPC
  kClientReq         // Client-to-server Rpc
};

// The client's key-value PUT request = the SMR command replicated in logs
struct client_req_t {
  size_t key[kAppKeySize / sizeof(size_t)];
  size_t value[kAppValueSize / sizeof(size_t)];

  std::string to_string() const {
    std::ostringstream ret;
    ret << "[Key (";
    for (size_t k : key) ret << std::to_string(k) << ", ";
    ret << "), Value (";
    for (size_t v : value) ret << std::to_string(v) << ", ";
    ret << ")]";
    return ret.str();
  }
};

// The client response message
enum class ClientRespType : size_t { kSuccess, kFailRedirect, kFailTryAgain };
struct client_resp_t {
  ClientRespType resp_type;
  int leader_node_id;  // ID of the leader node if resp type is kFailRedirect

  std::string to_string() const {
    switch (resp_type) {
      case ClientRespType::kSuccess: return "success";
      case ClientRespType::kFailRedirect:
        return "failed: redirect to node " + std::to_string(leader_node_id);
      case ClientRespType::kFailTryAgain: return "failed: try again";
    }
    return "Invalid";
  }

  client_resp_t(){};
  client_resp_t(ClientRespType resp_type) : resp_type(resp_type) {}
  client_resp_t(ClientRespType resp_type, int leader_node_id)
      : resp_type(resp_type), leader_node_id(leader_node_id) {}
};

class AppContext;  // Forward declaration

// Peer-peer or client-peer connection
struct connection_t {
  bool disconnected = false;  // True if this session is disconnected
  int session_num = -1;       // eRPC session number
  size_t session_idx = std::numeric_limits<size_t>::max();  // Index in conn_vec
  AppContext *c;
};

// Tag for requests sent to Raft peers (both requestvote and appendentries)
struct raft_req_tag_t {
  erpc::MsgBuffer req_msgbuf;
  erpc::MsgBuffer resp_msgbuf;
  raft_node_t *node;  // The Raft node to which req was sent
};

// Info about client request(s) saved at a leader for the nested Rpc. Each
// Raft server has one of these.
struct leader_saveinfo_t {
  bool in_use = false;          // Leader has an ongoing commit request
  erpc::ReqHandle *req_handle;  // This could be a vector if we do batching
  uint64_t start_tsc;           // Time at which client's request was received
  msg_entry_response_t msg_entry_response;  // Used to check commit status
};

// Context for both servers and clients
class AppContext {
 public:
  // Raft server members
  struct {
    int node_id = -1;  // This server's Raft node ID
    raft_server_t *raft = nullptr;
    size_t raft_periodic_tsc;           // rdtsc timestamp
    leader_saveinfo_t leader_saveinfo;  // Info for the ongoing commit request
    std::vector<TimeEnt> time_ents;

    // An in-memory pool for Raft entry data. In non-persistent mode, the Raft
    // log contains pointers to buffers allocated from this pool. In persistent
    // mode, these entries are copied to the DAX file.
    AppMemPool<client_req_t> log_entry_pool;

    // The presistent memory Raft log, used only if persistent memory is
    // enabled. This is a linear memory chunk that starts with persistent
    // metadata records.
    struct {
      uint8_t *p_buf;        // The start of the mapped file
      size_t mapped_len;     // Length of the mapped log file
      size_t v_num_entries;  // Volatile record for number of entries

      // Persistent metadata records
      raft_node_id_t *p_voted_for;  // Persistent record for persist-vote
      raft_term_t *p_term;          // Persistent record for perist-term
      size_t *p_num_entries;  // Persistent record for number of log entries

      // The persistent log
      uint8_t *p_log_base;
    } pmem;

    // The volatile in-memory Raft log, used only if persistent memory is
    // enabled. This is a vector of raft_entry_t entries. Each such entry has a
    // pointer to volatile log entries allocated from log_entry_pool.
    std::vector<raft_entry_t> dram_raft_log;

    // Request tags used for RPCs exchanged among Raft servers
    AppMemPool<raft_req_tag_t> raft_req_tag_pool;

    // App state
    FixedTable *table = nullptr;

    // Stats
    erpc::Latency commit_latency;            // Amplification factor = 10
    size_t stat_requestvote_enq_fail = 0;    // Failed to send requestvote req
    size_t stat_appendentries_enq_fail = 0;  // Failed to send appendentries req

    size_t get_num_log_entries() const {
      if (kUsePmem) return pmem.v_num_entries;
      return dram_raft_log.size();
    }
  } server;

  // SMR client members
  struct {
    size_t thread_id;
    size_t leader_idx;  // Client's view of the leader node's index in conn_vec
    size_t num_resps = 0;
    erpc::MsgBuffer req_msgbuf;   // Preallocated req msgbuf
    erpc::MsgBuffer resp_msgbuf;  // Preallocated response msgbuf

    // For latency measurement
    uint64_t req_start_tsc;
    std::vector<double> req_us_vec;  // We clear this after printing stats
  } client;

  // Common members
  std::vector<connection_t> conn_vec;
  erpc::Rpc<erpc::CTransport> *rpc = nullptr;
  erpc::FastRand fast_rand;
  size_t num_sm_resps = 0;
};

// Generate a deterministic, random-ish node ID for a process. Process IDs are
// unique at the cluster level. XXX: This can collide!
static int get_raft_node_id_for_process(size_t process_id) {
  std::string uri = erpc::get_uri_for_process(process_id);
  uint32_t hash = CityHash32(uri.c_str(), uri.length());
  return static_cast<int>(hash);
}

// eRPC session management handler
void sm_handler(int session_num, erpc::SmEventType sm_event_type,
                erpc::SmErrType sm_err_type, void *_context) {
  auto *c = static_cast<AppContext *>(_context);
  c->num_sm_resps++;

  if (!(sm_event_type == erpc::SmEventType::kConnected ||
        sm_event_type == erpc::SmEventType::kDisconnected)) {
    throw std::runtime_error("Received unexpected SM event.");
  }

  // The callback gives us the eRPC session number - get the index in conn_vec
  size_t session_idx = c->conn_vec.size();
  for (size_t i = 0; i < c->conn_vec.size(); i++) {
    if (c->conn_vec[i].session_num == session_num) session_idx = i;
  }
  erpc::rt_assert(session_idx < c->conn_vec.size(), "Invalid session number");

  if (sm_event_type == erpc::SmEventType::kDisconnected) {
    c->conn_vec[session_idx].disconnected = true;
  }

  fprintf(stderr,
          "smr: Rpc %u: Session number %d (index %zu) %s. Error = %s. "
          "Time elapsed = %.3f s.\n",
          c->rpc->get_rpc_id(), session_num, session_idx,
          erpc::sm_event_type_str(sm_event_type).c_str(),
          erpc::sm_err_type_str(sm_err_type).c_str(),
          c->rpc->sec_since_creation());
}

// Globals
std::unordered_map<int, std::string> node_id_to_name_map;

volatile sig_atomic_t ctrl_c_pressed = 0;
void ctrl_c_handler(int) { ctrl_c_pressed = 1; }

inline void call_raft_periodic(AppContext *c) {
  // raft_periodic() takes the number of msec elapsed since the last call. This
  // is done for ~100 msec timeouts, so this approximation is fine.
  size_t cur_tsc = erpc::rdtsc();

  // Assume TSC freqency is around 2.8 GHz. 1 ms = 2.8 * 100,000 ticks.
  bool msec_elapsed = (erpc::rdtsc() - c->server.raft_periodic_tsc > 2800000);

  if (msec_elapsed) {
    c->server.raft_periodic_tsc = cur_tsc;
    raft_periodic(c->server.raft, 1);
  } else {
    raft_periodic(c->server.raft, 0);
  }
}

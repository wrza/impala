// Copyright 2012 Cloudera Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "runtime/coordinator.h"

#include <limits>
#include <map>
#include <protocol/TBinaryProtocol.h>
#include <protocol/TDebugProtocol.h>
#include <transport/TTransportUtils.h>
#include <boost/algorithm/string/join.hpp>
#include <boost/accumulators/accumulators.hpp>
#include <boost/accumulators/statistics/stats.hpp>
#include <boost/accumulators/statistics/min.hpp>
#include <boost/accumulators/statistics/mean.hpp>
#include <boost/accumulators/statistics/median.hpp>
#include <boost/accumulators/statistics/max.hpp>
#include <boost/accumulators/statistics/variance.hpp>
#include <boost/bind.hpp>
#include <boost/foreach.hpp>
#include <boost/unordered_set.hpp>

#include "common/logging.h"
#include "exec/data-sink.h"
#include "runtime/client-cache.h"
#include "runtime/data-stream-sender.h"
#include "runtime/data-stream-mgr.h"
#include "runtime/exec-env.h"
#include "runtime/hdfs-fs-cache.h"
#include "runtime/plan-fragment-executor.h"
#include "runtime/row-batch.h"
#include "runtime/parallel-executor.h"
#include "sparrow/scheduler.h"
#include "exec/exec-stats.h"
#include "exec/data-sink.h"
#include "exec/scan-node.h"
#include "util/debug-util.h"
#include "util/hdfs-util.h"
#include "util/container-util.h"
#include "gen-cpp/ImpalaInternalService.h"
#include "gen-cpp/ImpalaInternalService_types.h"
#include "gen-cpp/Frontend_types.h"
#include "gen-cpp/PlanNodes_types.h"
#include "gen-cpp/Partitions_types.h"
#include "gen-cpp/JavaConstants_constants.h"

using namespace std;
using namespace boost;
using namespace boost::accumulators;
using namespace apache::thrift::transport;
using namespace apache::thrift;

DECLARE_int32(be_port);
DECLARE_string(ipaddress);
DECLARE_string(hostname);

namespace impala {

// Execution state of a particular fragment.
// Concurrent accesses:
// - GetNodeThroughput() called when coordinator's profile is printed
// - updates through UpdateFragmentExecStatus()
class Coordinator::BackendExecState {
 public:
  TUniqueId fragment_instance_id;
  WallClockStopWatch stopwatch;  // wall clock timer for this fragment
  const THostPort hostport;  // of ImpalaInternalService
  int64_t total_split_size;  // summed up across all splits; in bytes

  // assembled in c'tor
  TExecPlanFragmentParams rpc_params;

  // Fragment idx for this ExecState
  int fragment_idx;

  // protects fields below
  // lock ordering: Coordinator::lock_ can only get obtained *prior*
  // to lock
  boost::mutex lock;

  // if the status indicates an error status, execution of this fragment
  // has either been aborted by the remote backend (which then reported the error)
  // or cancellation has been initiated; either way, execution must not be cancelled
  Status status;

  bool initiated; // if true, TPlanExecRequest rpc has been sent
  bool done;  // if true, execution terminated; do not cancel in that case
  bool profile_created;  // true after the first call to profile->Update()
  RuntimeProfile* profile;  // owned by obj_pool()
  std::vector<std::string> error_log; // errors reported by this backend
  
  // Total scan ranges complete across all scan nodes
  int64_t total_ranges_complete;

  FragmentInstanceCounters aggregate_counters;
  
  BackendExecState(Coordinator* coord, const THostPort& coord_hostport,
      int backend_num, const TPlanFragment& fragment, int fragment_idx,
      const FragmentExecParams& params, int instance_idx, ObjectPool* obj_pool) 
    : fragment_instance_id(params.instance_ids[instance_idx]),
      hostport(params.hosts[instance_idx]),
      total_split_size(0),
      fragment_idx(fragment_idx),
      initiated(false),
      done(false),
      profile_created(false),
      total_ranges_complete(0) {
    profile = obj_pool->Add(
        new RuntimeProfile(obj_pool, "Instance " + PrintId(fragment_instance_id)));
    coord->SetExecPlanFragmentParams(backend_num, fragment, fragment_idx, params,
        instance_idx, coord_hostport, &rpc_params);
    ComputeTotalSplitSize();
  }

  // Computes sum of split sizes of leftmost scan. Call only after setting
  // exec_params.
  void ComputeTotalSplitSize();

  // Return value of throughput counter for given plan_node_id, or 0 if that node
  // doesn't exist.
  // Thread-safe.
  int64_t GetNodeThroughput(int plan_node_id);

  // Return number of completed scan ranges for plan_node_id, or 0 if that node
  // doesn't exist.
  // Thread-safe.
  int64_t GetNumScanRangesCompleted(int plan_node_id);

  // Updates the total number of scan ranges complete for this fragment.  Returns
  // the delta since the last time this was called.
  // lock must be taken before calling this.
  int64_t UpdateNumScanRangesCompleted();
};

void Coordinator::BackendExecState::ComputeTotalSplitSize() {
  const PerNodeScanRanges& per_node_scan_ranges = rpc_params.params.per_node_scan_ranges;
  total_split_size = 0;
  BOOST_FOREACH(const PerNodeScanRanges::value_type& entry, per_node_scan_ranges) {
    BOOST_FOREACH(const TScanRangeParams& scan_range_params, entry.second) {
      if (!scan_range_params.scan_range.__isset.hdfs_file_split) continue;
      total_split_size += scan_range_params.scan_range.hdfs_file_split.length;
    }
  }
}

int64_t Coordinator::BackendExecState::GetNodeThroughput(int plan_node_id) {
  RuntimeProfile::Counter* counter = NULL;
  {
    lock_guard<mutex> l(lock);
    CounterMap& throughput_counters = aggregate_counters.throughput_counters;
    CounterMap::iterator i = throughput_counters.find(plan_node_id);
    if (i == throughput_counters.end()) return 0;
    counter = i->second;
  }
  DCHECK(counter != NULL);
  // make sure not to hold lock when calling value() to avoid potential deadlocks
  return counter->value();
}

int64_t Coordinator::BackendExecState::GetNumScanRangesCompleted(int plan_node_id) {
  RuntimeProfile::Counter* counter = NULL;
  {
    lock_guard<mutex> l(lock);
    CounterMap& ranges_complete = aggregate_counters.scan_ranges_complete_counters;
    CounterMap::iterator i = ranges_complete.find(plan_node_id);
    if (i == ranges_complete.end()) return 0;
    counter = i->second;
  }
  DCHECK(counter != NULL);
  // make sure not to hold lock when calling value() to avoid potential deadlocks
  return counter->value();
}

int64_t Coordinator::BackendExecState::UpdateNumScanRangesCompleted() {
  int64_t total = 0;
  CounterMap& complete = aggregate_counters.scan_ranges_complete_counters;
  for (CounterMap::iterator i = complete.begin(); i != complete.end(); ++i) {
    total += i->second->value();
  } 
  int64_t delta = total - total_ranges_complete;
  total_ranges_complete = total;
  DCHECK_GE(delta, 0);
  return delta;
}

Coordinator::Coordinator(ExecEnv* exec_env, ExecStats* exec_stats)
  : exec_env_(exec_env),
    has_called_wait_(false),
    executor_(NULL), // Set in Prepare()
    exec_stats_(exec_stats),
    num_backends_(0),
    num_remaining_backends_(0),
    num_scan_ranges_(0),
    obj_pool_(NULL) {
}

Coordinator::~Coordinator() {
}

Status Coordinator::Exec(
    const TUniqueId& query_id, TQueryExecRequest* request,
    const TQueryOptions& query_options) {
  DCHECK_GT(request->fragments.size(), 0);
  needs_finalization_ = request->__isset.finalize_params;
  if (needs_finalization_) {
    finalize_params_ = request->finalize_params;
  }

  query_id_ = query_id;
  VLOG_QUERY << "Exec() query_id=" << query_id_;
  desc_tbl_ = request->desc_tbl;
  query_globals_ = request->query_globals;
  query_options_ = query_options;

  query_profile_.reset(new RuntimeProfile(obj_pool(), "Query " + PrintId(query_id_)));
  SCOPED_TIMER(query_profile_->total_time_counter());

  ComputeFragmentExecParams(*request);
  ComputeScanRangeAssignment(*request);

  THostPort coord;
  coord.__set_hostname(FLAGS_hostname);
  coord.__set_ipaddress(FLAGS_ipaddress);
  coord.__set_port(FLAGS_be_port);

  // to keep things simple, make async Cancel() calls wait until plan fragment
  // execution has been initiated, otherwise we might try to cancel fragment
  // execution at backends where it hasn't even started
  lock_guard<mutex> l(lock_);

  // we run the root fragment ourselves if it is unpartitioned
  bool has_coordinator_fragment =
      request->fragments[0].partition.type == TPartitionType::UNPARTITIONED;

  if (has_coordinator_fragment) {
    executor_.reset(new PlanFragmentExecutor(
            exec_env_, PlanFragmentExecutor::ReportStatusCallback()));
    // If a coordinator fragment is requested (for most queries this
    // will be the case, the exception is parallel INSERT queries), start
    // this before starting any more plan fragments in backend threads,
    // otherwise they start sending data before the local exchange node
    // had a chance to register with the stream mgr
    TExecPlanFragmentParams rpc_params;
    SetExecPlanFragmentParams(
        0, request->fragments[0], 0, fragment_exec_params_[0], 0, coord, &rpc_params);
    RETURN_IF_ERROR(executor_->Prepare(rpc_params));
  } else {
    executor_.reset(NULL);
    obj_pool_.reset(new ObjectPool());
  }

  // register coordinator's fragment profile now, before those of the backends,
  // so it shows up at the top
  aggregate_profile_ = obj_pool()->Add(
      new RuntimeProfile(obj_pool(), "Aggregate Profile"));
  query_profile_->AddChild(aggregate_profile_);
  if (executor_.get() != NULL) {
    query_profile_->AddChild(executor_->profile());
    executor_->profile()->set_name("Coordinator Fragment");
    CollectScanNodeCounters(executor_->profile(), &coordinator_counters_);
  }
  
  // Initialize per fragment profile data
  fragment_profiles_.resize(request->fragments.size());
  for (int i = 0; i < request->fragments.size(); ++i) {
    fragment_profiles_[i].num_instances = 0;
    
    // Special case fragment idx 0 if there is a coordinator. There is only one
    // instance of this profile so the average is just the coordinator profile. 
    if (i == 0 && has_coordinator_fragment) {
      fragment_profiles_[i].averaged_profile = executor_->profile();
      continue;
    }
    stringstream ss;
    ss << "Averaged Fragment " << i;
    fragment_profiles_[i].averaged_profile = 
        obj_pool()->Add(new RuntimeProfile(obj_pool(), ss.str()));
    // Insert the avg profile after the coordinator one or the aggregate
    // profile if there is no coordinator.
    if (executor_.get() != NULL) {
      query_profile_->AddChild(
          fragment_profiles_[i].averaged_profile, true, executor_->profile());
    } else {
      query_profile_->AddChild(
          fragment_profiles_[i].averaged_profile, true, aggregate_profile_);
    }
    
    ss.str("");
    ss << "Fragment " << i;
    fragment_profiles_[i].root_profile = 
        obj_pool()->Add(new RuntimeProfile(obj_pool(), ss.str()));
    query_profile_->AddChild(fragment_profiles_[i].root_profile);
  }

  // start fragment instances from left to right, so that receivers have
  // Prepare()'d before senders start sending
  backend_exec_states_.resize(num_backends_);
  num_remaining_backends_ = num_backends_;
  VLOG_QUERY << "starting " << num_backends_ << " backends for query " << query_id_;
  int backend_num = 0;
  
  for (int fragment_idx = (has_coordinator_fragment ? 1 : 0);
       fragment_idx < request->fragments.size(); ++fragment_idx) {
    FragmentExecParams& params = fragment_exec_params_[fragment_idx];

    // set up exec states
    int num_hosts = params.hosts.size();
    DCHECK_GT(num_hosts, 0);
    for (int instance_idx = 0; instance_idx < num_hosts; ++instance_idx) {
      // TODO: pool of pre-formatted BackendExecStates?
      BackendExecState* exec_state =
          obj_pool()->Add(new BackendExecState(this, coord, backend_num,
              request->fragments[fragment_idx], fragment_idx,
              params, instance_idx, obj_pool()));
      backend_exec_states_[backend_num] = exec_state;
      ++backend_num;
      VLOG(2) << "Exec(): starting instance: fragment_idx=" << fragment_idx
              << " instance_id=" << params.instance_ids[instance_idx];
    }
    fragment_profiles_[fragment_idx].num_instances = num_hosts;

    // Issue all rpcs in parallel
    Status fragments_exec_status = ParallelExecutor::Exec(
        bind<Status>(mem_fn(&Coordinator::ExecRemoteFragment), this, _1), 
        reinterpret_cast<void**>(&backend_exec_states_[backend_num - num_hosts]),
        num_hosts);

    if (!fragments_exec_status.ok()) {
      DCHECK(query_status_.ok());  // nobody should have been able to cancel
      query_status_ = fragments_exec_status;
      // tear down running fragments and return
      CancelInternal();
      return fragments_exec_status;
    }
  }

  PrintBackendInfo();

  stringstream ss;
  ss << "Query " << query_id_;
  progress_ = ProgressUpdater(ss.str(), num_scan_ranges_);
  progress_.set_logging_level(1);

  return Status::OK;
}

Status Coordinator::GetStatus() {
  lock_guard<mutex> l(lock_);
  return query_status_;
}

Status Coordinator::UpdateStatus(const Status& status, const TUniqueId* id) {
  {
    lock_guard<mutex> l(lock_);
    // nothing to update
    if (status.ok()) return query_status_;

    // don't override an error status; also, cancellation has already started
    if (!query_status_.ok()) return query_status_;
  
    query_status_ = status;
    CancelInternal();
  }

  // Log the id of the fragment that first failed so we can track it down easier.
  if (id != NULL) {
    VLOG_QUERY << "Query id=" << query_id_ << " failed because fragment id="
               << *id << " failed.";
  }

  return query_status_;
}

Status Coordinator::FinalizeQuery() {
  // All backends must have reported their final statuses before finalization,
  // which is a post-condition of Wait.
  DCHECK(has_called_wait_);
  DCHECK(needs_finalization_);

  hdfsFS hdfs_connection = exec_env_->fs_cache()->GetDefaultConnection();

  // TODO: If this process fails, the state of the table's data is left
  // undefined. We should do better cleanup: there's probably enough information
  // here to roll back to the table's previous state.

  // INSERT finalization happens in the four following steps
  // 1. If OVERWRITE, remove all the files in the target directory
  // 2. Create all the necessary partition directories.
  BOOST_FOREACH(const PartitionRowCount::value_type& partition, 
      partition_row_counts_) {
    stringstream ss;
    // Fully-qualified partition path
    ss << finalize_params_.hdfs_base_dir << "/" << partition.first;
    if (finalize_params_.is_overwrite) {   
      if (partition.first.empty()) {
        // If the root directory is written to, then the table must not be partitioned
        DCHECK(partition_row_counts_.size() == 1);
        // We need to be a little more careful, and only delete data files in the root
        // because the tmp directories the sink(s) wrote are there also. 
        // So only delete files in the table directory - all files are treated as data 
        // files by Hive and Impala, but directories are ignored (and may legitimately
        // be used to store permanent non-table data by other applications). 
        int num_files = 0;
        hdfsFileInfo* existing_files = 
            hdfsListDirectory(hdfs_connection, ss.str().c_str(), &num_files);
        if (existing_files == NULL) {
          return AppendHdfsErrorMessage("Could not list directory: ", ss.str());
        }
        Status delete_status = Status::OK;
        for (int i = 0; i < num_files; ++i) {
          if (existing_files[i].mKind == kObjectKindFile) {
            VLOG(2) << "Deleting: " << string(existing_files[i].mName);
            if (hdfsDelete(hdfs_connection, existing_files[i].mName, 1) == -1) {
              delete_status = Status(AppendHdfsErrorMessage("Failed to delete existing "
                  "HDFS file as part of INSERT OVERWRITE query: ", 
                  string(existing_files[i].mName)));
              break;
            }
          }
        }
        hdfsFreeFileInfo(existing_files, num_files);
        RETURN_IF_ERROR(delete_status);
      } else {
        // This is a partition directory, not the root directory; we can delete
        // recursively with abandon, after checking it was ever created. 
        if (hdfsExists(hdfs_connection, ss.str().c_str()) != -1) {
          // TODO: There's a potential race here between checking for the directory
          // and a third-party deleting it. 
          if (hdfsDelete(hdfs_connection, ss.str().c_str(), 1) == -1) {
            return Status(AppendHdfsErrorMessage("Failed to delete partition directory "
                    "as part of INSERT OVERWRITE query: ", ss.str()));
          }
        }
      }
    }
    // Ignore error if directory already exists
    hdfsCreateDirectory(hdfs_connection, ss.str().c_str());
  }

  // 3. Move all tmp files
  set<string> tmp_dirs_to_delete;
  BOOST_FOREACH(FileMoveMap::value_type& move, files_to_move_) {
    // Empty destination means delete (which we do in a separate
    // pass because we may not have processed the contents of this
    // dir yet)
    if (move.second.empty()) {
      tmp_dirs_to_delete.insert(move.first);
    } else {
      VLOG_ROW << "Moving tmp file: " << move.first << " to " << move.second;
      if (hdfsRename(hdfs_connection, move.first.c_str(), move.second.c_str()) == -1) {
        stringstream ss;
        ss << "Could not move HDFS file: " << move.first << " to desintation: " 
           << move.second;
        return AppendHdfsErrorMessage(ss.str());
      }          
    }
  }

  // 4. Delete temp directories
  BOOST_FOREACH(const string& tmp_path, tmp_dirs_to_delete) {
    if (hdfsDelete(hdfs_connection, tmp_path.c_str(), 1) == -1) {
      return Status(AppendHdfsErrorMessage("Failed to delete temporary directory: ", 
          tmp_path));
    }
  }

  return Status::OK;
}

Status Coordinator::WaitForAllBackends() {
  unique_lock<mutex> l(lock_);
  VLOG_QUERY << "Coordinator waiting for backends to finish, " 
             << num_remaining_backends_ << " remaining";
  while (num_remaining_backends_ > 0 && query_status_.ok()) {
    backend_completion_cv_.wait(l);
  }
  VLOG_QUERY << "All backends finished or error.";    
 
  return query_status_;
}

Status Coordinator::Wait() {
  lock_guard<mutex> l(wait_lock_);
  if (has_called_wait_) return Status::OK;
  has_called_wait_ = true;
  if (executor_.get() != NULL) {
    // Open() may block
    RETURN_IF_ERROR(UpdateStatus(executor_->Open(), NULL));

    // If the coordinator fragment has a sink, it will have finished executing at this
    // point.  It's safe therefore to copy the set of files to move and updated partitions
    // into the query-wide set.
    RuntimeState* state = runtime_state();
    DCHECK(state != NULL);

    // No other backends should have updated these structures if the coordinator has a
    // fragment.  (Backends have a sink only if the coordinator does not)
    DCHECK_EQ(files_to_move_.size(), 0);
    DCHECK_EQ(partition_row_counts_.size(), 0);

    // Because there are no other updates, safe to copy the maps rather than merge them.
    files_to_move_ = *state->hdfs_files_to_move();
    partition_row_counts_ = *state->num_appended_rows();
  } else {
    // Query finalization can only happen when all backends have reported
    // relevant state. They only have relevant state to report in the parallel
    // INSERT case, otherwise all the relevant state is from the coordinator
    // fragment which will be available after Open() returns. 
    RETURN_IF_ERROR(WaitForAllBackends());
  }

  // Query finalization is required only for HDFS table sinks
  if (needs_finalization_) {
    return FinalizeQuery();
  }
  
  return Status::OK;
}

Status Coordinator::GetNext(RowBatch** batch, RuntimeState* state) {
  VLOG_ROW << "GetNext() query_id=" << query_id_;
  DCHECK(has_called_wait_);
  SCOPED_TIMER(query_profile_->total_time_counter());

  if (executor_.get() == NULL) {
    // If there is no local fragment, we produce no output, and execution will
    // have finished after Wait.
    *batch = NULL;
    return GetStatus();
  }

  // do not acquire lock_ here, otherwise we could block and prevent an async
  // Cancel() from proceeding
  Status status = executor_->GetNext(batch);

  // if there was an error, we need to return the query's error status rather than
  // the status we just got back from the local executor (which may well be CANCELLED
  // in that case).  Coordinator fragment failed in this case so we log the query_id.
  RETURN_IF_ERROR(UpdateStatus(status, &runtime_state()->fragment_instance_id()));

  if (*batch == NULL) {
    // Don't return final NULL until all backends have completed.
    // GetNext must wait for all backends to complete before
    // ultimately signalling the end of execution via a NULL
    // batch. After NULL is returned, the coordinator may tear down
    // query state, and perform post-query finalization which might
    // depend on the reports from all backends.
    RETURN_IF_ERROR(WaitForAllBackends());
    if (query_status_.ok()) {
      // If the query completed successfully, report aggregate query profiles.
      ReportQuerySummary();
    }
  } else {
    exec_stats_->num_rows_ += (*batch)->num_rows();
  }
  return Status::OK;
}

void Coordinator::PrintBackendInfo() {
  for (int i = 0; i < backend_exec_states_.size(); ++i) {
    SummaryStats& acc = 
        fragment_profiles_[backend_exec_states_[i]->fragment_idx].bytes_assigned;
    acc(backend_exec_states_[i]->total_split_size);
  }

  for (int i = (executor_.get() == NULL ? 0 : 1); i < fragment_profiles_.size(); ++i) {
    SummaryStats& acc = fragment_profiles_[i].bytes_assigned;
    double min = accumulators::min(acc);
    double max = accumulators::max(acc);
    double mean = accumulators::mean(acc);
    double stddev = sqrt(accumulators::variance(acc));
    stringstream ss;
    ss << " min: " << PrettyPrinter::Print(min, TCounterType::BYTES)
      << ", max: " << PrettyPrinter::Print(max, TCounterType::BYTES)
      << ", avg: " << PrettyPrinter::Print(mean, TCounterType::BYTES)
      << ", stddev: " << PrettyPrinter::Print(stddev, TCounterType::BYTES);
    fragment_profiles_[i].averaged_profile->AddInfoString("split sizes", ss.str());

    if (VLOG_FILE_IS_ON) {
      VLOG_FILE << "Byte split for fragment " << i << " " << ss.str();
      for (int j = 0; j < backend_exec_states_.size(); ++j) {
        BackendExecState* exec_state = backend_exec_states_[j];
        if (exec_state->fragment_idx != i) continue;
        VLOG_FILE << "data volume for ipaddress " << exec_state << ": "
                  << PrettyPrinter::Print(
                    exec_state->total_split_size, TCounterType::BYTES);
      }
    }
  }
}

void Coordinator::CollectScanNodeCounters(RuntimeProfile* profile, 
    FragmentInstanceCounters* counters) {
  vector<RuntimeProfile*> children;
  profile->GetAllChildren(&children);
  for (int i = 0; i < children.size(); ++i) {
    RuntimeProfile* p = children[i];
    PlanNodeId id = ExecNode::GetNodeIdFromProfile(p);
    
    // This profile is not for an exec node.
    if (id == g_JavaConstants_constants.INVALID_PLAN_NODE_ID) continue;

    RuntimeProfile::Counter* throughput_counter = 
        p->GetCounter(ScanNode::TOTAL_THROUGHPUT_COUNTER);
    if (throughput_counter != NULL) {
      counters->throughput_counters[id] = throughput_counter;
    }
    RuntimeProfile::Counter* scan_ranges_counter = 
        p->GetCounter(ScanNode::SCAN_RANGES_COMPLETE_COUNTER);
    if (scan_ranges_counter != NULL) {
      counters->scan_ranges_complete_counters[id] = scan_ranges_counter;
    }
  }
}

void Coordinator::CreateAggregateCounters(
    const vector<TPlanFragment>& fragments) {
  BOOST_FOREACH(const TPlanFragment& fragment, fragments) {
    if (!fragment.__isset.plan) continue;
    const vector<TPlanNode>& nodes = fragment.plan.nodes;
    BOOST_FOREACH(const TPlanNode& node, nodes) {
      if (node.node_type != TPlanNodeType::HDFS_SCAN_NODE
          && node.node_type != TPlanNodeType::HBASE_SCAN_NODE) {
        continue;
      }

      stringstream s;
      s << PrintPlanNodeType(node.node_type) << " (id=" 
        << node.node_id << ") Throughput";
      aggregate_profile_->AddDerivedCounter(s.str(), TCounterType::BYTES_PER_SECOND,
          bind<int64_t>(mem_fn(&Coordinator::ComputeTotalThroughput),
                        this, node.node_id));
      s.str("");
      s << PrintPlanNodeType(node.node_type) << " (id=" 
        << node.node_id << ") Completed scan ranges";
      aggregate_profile_->AddDerivedCounter(s.str(), TCounterType::UNIT,
          bind<int64_t>(mem_fn(&Coordinator::ComputeTotalScanRangesComplete),
                        this, node.node_id));
    }
  }
}

int64_t Coordinator::ComputeTotalThroughput(int node_id) {
  int64_t value = 0;
  for (int i = 0; i < backend_exec_states_.size(); ++i) {
    BackendExecState* exec_state = backend_exec_states_[i];
    value += exec_state->GetNodeThroughput(node_id);
  }
  // Add up the local fragment throughput counter
  CounterMap& throughput_counters = coordinator_counters_.throughput_counters;
  CounterMap::iterator it = throughput_counters.find(node_id);
  if (it != throughput_counters.end()) {
    value += it->second->value();
  }
  return value;
}

int64_t Coordinator::ComputeTotalScanRangesComplete(int node_id) {
  int64_t value = 0;
  for (int i = 0; i < backend_exec_states_.size(); ++i) {
    BackendExecState* exec_state = backend_exec_states_[i];
    value += exec_state->GetNumScanRangesCompleted(node_id);
  }
  // Add up the local fragment throughput counter
  CounterMap& scan_ranges_complete = coordinator_counters_.scan_ranges_complete_counters;
  CounterMap::iterator it = scan_ranges_complete.find(node_id);
  if (it != scan_ranges_complete.end()) {
    value += it->second->value();
  }
  return value;
}

Status Coordinator::ExecRemoteFragment(void* exec_state_arg) {
  BackendExecState* exec_state = reinterpret_cast<BackendExecState*>(exec_state_arg);
  VLOG_FILE << "making rpc: ExecPlanFragment query_id=" << query_id_
            << " instance_id=" << exec_state->fragment_instance_id
            << " host=" << exec_state->hostport;
  lock_guard<mutex> l(exec_state->lock);

  // this client needs to have been released when this function finishes
  ImpalaInternalServiceClient* backend_client;
  // TODO: Fix the THostPort mess, make the client cache use the same type.
  pair<string, int> hostport = make_pair(exec_state->hostport.ipaddress, 
                                         exec_state->hostport.port);
  RETURN_IF_ERROR(exec_env_->client_cache()->GetClient(hostport, &backend_client));
  DCHECK(backend_client != NULL);

  TExecPlanFragmentResult thrift_result;
  try {
    try {
      backend_client->ExecPlanFragment(thrift_result, exec_state->rpc_params);
    } catch (TTransportException& e) {
      // If a backend has stopped and restarted (without the failure detector
      // picking it up) an existing backend client may still think it is
      // connected. To avoid failing the first query after every failure, catch
      // the first failure and force a reopen of the transport.
      // TODO: Improve client-cache so that we don't need to do this.
      VLOG_RPC << "Retrying ExecPlanFragment: " << e.what();
      Status status = exec_env_->client_cache()->ReopenClient(backend_client);
      if (!status.ok()) {
        exec_env_->client_cache()->ReleaseClient(backend_client);
        return status;
      }
      backend_client->ExecPlanFragment(thrift_result, exec_state->rpc_params);
    }
  } catch (TTransportException& e) {
    stringstream msg;
    msg << "ExecPlanRequest rpc query_id=" << query_id_
        << " instance_id=" << exec_state->fragment_instance_id 
        << " failed: " << e.what();
    VLOG_QUERY << msg.str();
    exec_state->status = Status(msg.str());
    exec_env_->client_cache()->ReleaseClient(backend_client);
    return exec_state->status;
  }
  exec_state->status = thrift_result.status;
  exec_env_->client_cache()->ReleaseClient(backend_client);
  if (exec_state->status.ok()) {
    exec_state->initiated = true;
    exec_state->stopwatch.Start();
  }
  return exec_state->status;
}

void Coordinator::Cancel() {
  lock_guard<mutex> l(lock_);
  // if the query status indicates an error, cancellation has already been initiated
  if (!query_status_.ok()) return;
  // prevent others from cancelling a second time
  query_status_ = Status::CANCELLED;
  CancelInternal();
}

void Coordinator::CancelInternal() {
  VLOG_QUERY << "Cancel() query_id=" << query_id_;
  DCHECK(!query_status_.ok());

  // cancel local fragment
  if (executor_.get() != NULL) executor_->Cancel();

  for (int i = 0; i < backend_exec_states_.size(); ++i) {
    BackendExecState* exec_state = backend_exec_states_[i];

    // lock each exec_state individually to synchronize correctly with
    // UpdateFragmentExecStatus() (which doesn't get the global lock_
    // to set its status)
    lock_guard<mutex> l(exec_state->lock);

    // no need to cancel if we already know it terminated w/ an error status
    if (!exec_state->status.ok()) continue;

    // set an error status to make sure we only cancel this once
    exec_state->status = Status::CANCELLED;

    // Nothing to cancel if the exec rpc was not sent
    if (!exec_state->initiated) continue;

    // don't cancel if it already finished
    if (exec_state->done) continue;

    // if we get an error while trying to get a connection to the backend,
    // keep going
    ImpalaInternalServiceClient* backend_client;
    pair<string, int> hostport = make_pair(exec_state->hostport.ipaddress, 
                                           exec_state->hostport.port);
    Status status =
        exec_env_->client_cache()->GetClient(hostport, &backend_client);
    if (!status.ok()) {
      continue;
    }
    DCHECK(backend_client != NULL);

    TCancelPlanFragmentParams params;
    params.protocol_version = ImpalaInternalServiceVersion::V1;
    params.__set_fragment_instance_id(exec_state->fragment_instance_id);
    TCancelPlanFragmentResult res;
    try {
      VLOG_QUERY << "sending CancelPlanFragment rpc for instance_id="
                 << exec_state->fragment_instance_id << " backend="
                 << exec_state->hostport;
      try { 
        backend_client->CancelPlanFragment(res, params);
      } catch (TTransportException& e) {
        VLOG_RPC << "Retrying CancelPlanFragment: " << e.what();
        Status status = exec_env_->client_cache()->ReopenClient(backend_client);
        if (!status.ok()) {
          exec_state->status.AddError(status);
          exec_env_->client_cache()->ReleaseClient(backend_client);
          continue;
        }
        backend_client->CancelPlanFragment(res, params);
      }
    } catch (TTransportException& e) {
      stringstream msg;
      msg << "CancelPlanFragment rpc query_id=" << query_id_
          << " instance_id=" << exec_state->fragment_instance_id 
          << " failed: " << e.what();
      // make a note of the error status, but keep on cancelling the other fragments
      exec_state->status.AddErrorMsg(msg.str());
      exec_env_->client_cache()->ReleaseClient(backend_client);
      continue;
    }
    if (res.status.status_code != TStatusCode::OK) {
      exec_state->status.AddErrorMsg(algorithm::join(res.status.error_msgs, "; "));
    }

    exec_env_->client_cache()->ReleaseClient(backend_client);
  }

  // notify that we completed with an error
  backend_completion_cv_.notify_all();

  // Report the summary with whatever progress the query made before being cancelled.
  ReportQuerySummary();
}

Status Coordinator::UpdateFragmentExecStatus(const TReportExecStatusParams& params) {
  VLOG_FILE << "UpdateFragmentExecStatus() query_id=" << query_id_
            << " status=" << params.status.status_code
            << " done=" << (params.done ? "true" : "false");
  if (params.backend_num >= backend_exec_states_.size()) {
    return Status(TStatusCode::INTERNAL_ERROR, "unknown backend number");
  }
  BackendExecState* exec_state = backend_exec_states_[params.backend_num];

  const TRuntimeProfileTree& cumulative_profile = params.profile;
  Status status(params.status);
  {
    lock_guard<mutex> l(exec_state->lock);
    // make sure we don't go from error status to OK
    DCHECK(!status.ok() || exec_state->status.ok())
        << "fragment is transitioning from error status to OK:"
        << " query_id=" << query_id_ << " instance_id="
        << exec_state->fragment_instance_id
        << " status=" << exec_state->status.GetErrorMsg();
    exec_state->status = status;
    exec_state->done = params.done;
    RuntimeProfile* p = RuntimeProfile::CreateFromThrift(obj_pool(), cumulative_profile);
    stringstream str;
    p->PrettyPrint(&str);
    exec_state->profile->Update(cumulative_profile);
    if (!exec_state->profile_created) {
      CollectScanNodeCounters(exec_state->profile, &exec_state->aggregate_counters);
    }
    exec_state->profile_created = true;

    if (params.__isset.error_log && params.error_log.size() > 0) {
      exec_state->error_log.insert(exec_state->error_log.end(), params.error_log.begin(),
          params.error_log.end());
      VLOG_FILE << "instance_id=" << exec_state->fragment_instance_id
                << " error log: " << join(exec_state->error_log, "\n");
    }
    progress_.Update(exec_state->UpdateNumScanRangesCompleted());
  }

  if (params.done && params.__isset.insert_exec_status) {
    lock_guard<mutex> l(lock_);
    // Merge in table update data (partitions written to, files to be moved as part of
    // finalization)
    
    BOOST_FOREACH(const PartitionRowCount::value_type& partition, 
        params.insert_exec_status.num_appended_rows) {
      partition_row_counts_[partition.first] += partition.second;
    }
    files_to_move_.insert(
        params.insert_exec_status.files_to_move.begin(),
        params.insert_exec_status.files_to_move.end());
  }

  if (VLOG_FILE_IS_ON) {
    stringstream s;
    exec_state->profile->PrettyPrint(&s);
    VLOG_FILE << "profile for query_id=" << query_id_
               << " instance_id=" << exec_state->fragment_instance_id
               << "\n" << s.str();
  }
  // also print the cumulative profile
  // TODO: fix the coordinator/PlanFragmentExecutor, so this isn't needed
  if (VLOG_FILE_IS_ON) {
    stringstream s;
    query_profile_->PrettyPrint(&s);
    VLOG_FILE << "cumulative profile for query_id=" << query_id_ 
              << "\n" << s.str();
  }

  // for now, abort the query if we see any error
  // (UpdateStatus() initiates cancellation, if it hasn't already been initiated)
  if (!status.ok()) {
    UpdateStatus(status, &exec_state->fragment_instance_id);
    return Status::OK;
  }

  if (params.done) {
    lock_guard<mutex> l(lock_);
    exec_state->stopwatch.Stop();
    DCHECK_GT(num_remaining_backends_, 0);
    VLOG_QUERY << "Backend " << params.backend_num << " completed, " 
               << num_remaining_backends_ - 1 << " remaining: query_id=" << query_id_;
    if (VLOG_QUERY_IS_ON && num_remaining_backends_ > 1) {
      // print host/port info for the first backend that's still in progress as a
      // debugging aid for backend deadlocks
      for (int i = 0; i < backend_exec_states_.size(); ++i) {
        BackendExecState* exec_state = backend_exec_states_[i];
        lock_guard<mutex> l2(exec_state->lock);
        if (!exec_state->done) {
          VLOG_QUERY << "query_id=" << query_id_ << ": first in-progress backend: "
                     << exec_state->hostport.ipaddress << ":" << exec_state->hostport.port;
          break;
        }
      }
    }
    if (--num_remaining_backends_ == 0) {
      backend_completion_cv_.notify_all();
    }
  }

  return Status::OK;
}

const RowDescriptor& Coordinator::row_desc() const {
  DCHECK(executor_.get() != NULL);
  return executor_->row_desc();
}

RuntimeState* Coordinator::runtime_state() {
  return executor_.get() == NULL ? NULL : executor_->runtime_state();
}

ObjectPool* Coordinator::obj_pool() {
  return executor_.get() == NULL ? obj_pool_.get() : 
    executor_->runtime_state()->obj_pool();
}

bool Coordinator::PrepareCatalogUpdate(TCatalogUpdate* catalog_update) {
  // Assume we are called only after all fragments have completed
  DCHECK(has_called_wait_);

  BOOST_FOREACH(const PartitionRowCount::value_type& partition,
      partition_row_counts_) {
    catalog_update->created_partitions.insert(partition.first);
  }

  return catalog_update->created_partitions.size() != 0;
}

// This function appends summary information to the query_profile_ before
// outputting it to VLOG.  It adds:
//   1. Averaged remote fragment profiles (TODO: add outliers)
//   2. Summary of remote fragment durations (min, max, mean, stddev)
//   3. Summary of remote fragment rates (min, max, mean, stddev)
// TODO: add histogram/percentile
void Coordinator::ReportQuerySummary() {
  // In this case, the query did not even get to start on all the remote nodes,
  // some of the state that is used below might be uninitialized.  In this case,
  // the query has made so little progress, reporting a summary is not very useful.
  if (!has_called_wait_) return;
  
  // The fragment has finished executing.  Update the profile to compute the 
  // fraction of time spent in each node.
  if (executor_.get() != NULL) executor_->profile()->ComputeTimeInProfile();

  if (!backend_exec_states_.empty()) {
    // Average all remote fragments for each fragment.  
    for (int i = 0; i < backend_exec_states_.size(); ++i) {
      backend_exec_states_[i]->profile->ComputeTimeInProfile();
      
      int fragment_idx = backend_exec_states_[i]->fragment_idx;
      DCHECK_GE(fragment_idx, 0);
      DCHECK_LT(fragment_idx, fragment_profiles_.size());
      PerFragmentProfileData& data = fragment_profiles_[fragment_idx];

      int64_t completion_time = backend_exec_states_[i]->stopwatch.ElapsedTime();
      data.completion_times(completion_time);
      data.rates(backend_exec_states_[i]->total_split_size / (completion_time / 1000.0));
      data.averaged_profile->Merge(backend_exec_states_[i]->profile);
      data.root_profile->AddChild(backend_exec_states_[i]->profile);
    }
    
    // Per fragment instances have been collected, output summaries
    for (int i = (executor_.get() != NULL ? 1 : 0); i < fragment_profiles_.size(); ++i) {
      RuntimeProfile* profile = fragment_profiles_[i].averaged_profile;
      profile->Divide(fragment_profiles_[i].num_instances);

      SummaryStats& completion_times = fragment_profiles_[i].completion_times;
      SummaryStats& rates = fragment_profiles_[i].rates;
      
      stringstream times_label;
      times_label 
        << "min:" << PrettyPrinter::Print(
            accumulators::min(completion_times), TCounterType::TIME_MS)
        << "  max:" << PrettyPrinter::Print(
            accumulators::max(completion_times), TCounterType::TIME_MS)
        << "  mean: " << PrettyPrinter::Print(
            accumulators::mean(completion_times), TCounterType::TIME_MS)
        << "  stddev:" << PrettyPrinter::Print(
            sqrt(accumulators::variance(completion_times)), TCounterType::TIME_MS);

      stringstream rates_label;
      rates_label 
        << "min:" << PrettyPrinter::Print(
            accumulators::min(rates), TCounterType::BYTES_PER_SECOND)
        << "  max:" << PrettyPrinter::Print(
            accumulators::max(rates), TCounterType::BYTES_PER_SECOND)
        << "  mean:" << PrettyPrinter::Print(
            accumulators::mean(rates), TCounterType::BYTES_PER_SECOND)
        << "  stddev:" << PrettyPrinter::Print(
            sqrt(accumulators::variance(rates)), TCounterType::BYTES_PER_SECOND);

      fragment_profiles_[i].averaged_profile->AddInfoString(
          "completion times", times_label.str());
      fragment_profiles_[i].averaged_profile->AddInfoString(
          "execution rates", rates_label.str());
    }
  } 

  if (VLOG_QUERY_IS_ON) {
    stringstream ss;
    ss << "Final profile for query_id=" << query_id_ << endl;
    query_profile_->PrettyPrint(&ss);
    VLOG_QUERY << ss.str();
  }
}

string Coordinator::GetErrorLog() {
  stringstream ss;
  lock_guard<mutex> l(lock_);
  if (executor_.get() != NULL && executor_->runtime_state() != NULL &&
      !executor_->runtime_state()->ErrorLogIsEmpty()) {
    ss << executor_->runtime_state()->ErrorLog() << "\n";
  }
  for (int i = 0; i < backend_exec_states_.size(); ++i) {
    lock_guard<mutex> l(backend_exec_states_[i]->lock);
    if (backend_exec_states_[i]->error_log.size() > 0) {
      ss << "Backend " << i << ":"
         << join(backend_exec_states_[i]->error_log, "\n") << "\n";
    }
  }
  return ss.str();
}

void Coordinator::ComputeFragmentExecParams(const TQueryExecRequest& exec_request) {
  fragment_exec_params_.resize(exec_request.fragments.size());
  ComputeFragmentHosts(exec_request);

  // assign instance ids
  BOOST_FOREACH(FragmentExecParams& params, fragment_exec_params_) {
    for (int j = 0; j < params.hosts.size(); ++j) {
      int instance_num = num_backends_ + j;
      // we add instance_num to query_id.lo to create a globally-unique instance id
      TUniqueId instance_id;
      instance_id.hi = query_id_.hi;
      DCHECK_LT(
          query_id_.lo, numeric_limits<int64_t>::max() - instance_num - 1);
      instance_id.lo = query_id_.lo + instance_num + 1;
      params.instance_ids.push_back(instance_id);
    }
    num_backends_ += params.hosts.size();
  }
  if (exec_request.fragments[0].partition.type == TPartitionType::UNPARTITIONED) {
    // the root fragment is executed directly by the coordinator
    --num_backends_;
  }

  // compute destinations and # senders per exchange node
  // (the root fragment doesn't have a destination)
  for (int i = 1; i < fragment_exec_params_.size(); ++i) {
    FragmentExecParams& params = fragment_exec_params_[i];
    int dest_fragment_idx = exec_request.dest_fragment_idx[i - 1];
    DCHECK_LT(dest_fragment_idx, fragment_exec_params_.size());
    FragmentExecParams& dest_params = fragment_exec_params_[dest_fragment_idx];

    // set # of senders
    DCHECK(exec_request.fragments[i].output_sink.__isset.stream_sink);
    const TDataStreamSink& sink = exec_request.fragments[i].output_sink.stream_sink;
    // we can only handle unpartitioned (= broadcast) output at the moment
    DCHECK(sink.output_partition.type == TPartitionType::UNPARTITIONED);
    PlanNodeId exch_id = sink.dest_node_id;
    // we might have multiple fragments sending to this exchange node 
    // (distributed MERGE), which is why we need to add up the #senders
    dest_params.per_exch_num_senders[exch_id] += params.hosts.size();

    // create one TPlanFragmentDestination per destination host
    params.destinations.resize(dest_params.hosts.size());
    for (int j = 0; j < dest_params.hosts.size(); ++j) {
      TPlanFragmentDestination& dest = params.destinations[j];
      dest.fragment_instance_id = dest_params.instance_ids[j];
      dest.server = THostPort(dest_params.hosts[j]);
      VLOG_RPC  << "dest for fragment " << i << ":"
                << " instance_id=" << dest.fragment_instance_id 
                << " server=" << dest.server.ipaddress << ":" << dest.server.port;
    }
  }
}

Status Coordinator::ComputeFragmentHosts(const TQueryExecRequest& exec_request) {
  THostPort coord;
  coord.ipaddress = coord.hostname = FLAGS_ipaddress;
  coord.port = FLAGS_be_port;
  DCHECK_EQ(fragment_exec_params_.size(), exec_request.fragments.size());
  vector<TPlanNodeType::type> scan_node_types;
  scan_node_types.push_back(TPlanNodeType::HDFS_SCAN_NODE);
  scan_node_types.push_back(TPlanNodeType::HBASE_SCAN_NODE);

  // compute hosts of producer fragment before those of consumer fragment(s),
  // the latter might inherit the set of hosts from the former
  for (int i = exec_request.fragments.size() - 1; i >= 0; --i) {
    const TPlanFragment& fragment = exec_request.fragments[i];
    FragmentExecParams& params = fragment_exec_params_[i];
    if (fragment.partition.type == TPartitionType::UNPARTITIONED) {
      // all single-node fragments run on the coordinator host
      params.hosts.push_back(coord);
      continue;
    }

    PlanNodeId leftmost_scan_id = FindLeftmostNode(fragment.plan, scan_node_types);
    if (leftmost_scan_id == g_JavaConstants_constants.INVALID_PLAN_NODE_ID) {
      // there is no leftmost scan; we assign the same hosts as those of our
      // leftmost input fragment (so that a partitioned aggregation fragment
      // runs on the hosts that provide the input data)
      int input_fragment_idx = FindLeftmostInputFragment(i, exec_request);
      DCHECK_GE(input_fragment_idx, 0);
      DCHECK_LT(input_fragment_idx, fragment_exec_params_.size());
      params.hosts = fragment_exec_params_[input_fragment_idx].hosts;
      // TODO: switch to unpartitioned/coord execution if our input fragment
      // is executed that way (could have been downgraded from distributed)
      continue;
    }

    map<TPlanNodeId, vector<TScanRangeLocations> >::const_iterator entry =
        exec_request.per_node_scan_ranges.find(leftmost_scan_id);
    if (entry == exec_request.per_node_scan_ranges.end() || entry->second.empty()) {
      // this scan node doesn't have any scan ranges; run it on the coordinator
      // TODO: we'll need to revisit this strategy once we can partition joins
      // (in which case this fragment might be executing a right outer join
      // with a large build table)
      params.hosts.push_back(coord);
      continue;
    }
    const vector<TScanRangeLocations>& scan_range_locations = entry->second;

    // collect unique set of data hosts
    unordered_set<const THostPort*, HashTHostPortPtr, THostPortPtrEquals> data_hosts;
    BOOST_FOREACH(const TScanRangeLocations& locations, scan_range_locations) {
      BOOST_FOREACH(const TScanRangeLocation& location, locations.locations) {
        data_hosts.insert(&(location.server));
      }
    }
    vector<THostPort> data_hostports;
    BOOST_FOREACH(const THostPort* host_port, data_hosts) {
      data_hostports.push_back(*host_port);
    }

    // find execution hosts for data hosts
    RETURN_IF_ERROR(exec_env_->scheduler()->GetHosts(data_hostports, &params.hosts));
    DCHECK_EQ(data_hostports.size(), params.hosts.size());
    for (int j = 0; j < data_hostports.size(); ++j) {
      params.data_server_map[data_hostports[j]] = params.hosts[j];
    }

    // de-dup
    sort(params.hosts.begin(), params.hosts.end());
    vector<THostPort >::iterator start_duplicates = 
        unique(params.hosts.begin(), params.hosts.end());
    params.hosts.erase(start_duplicates, params.hosts.end());
    unique_hosts_.insert(params.hosts.begin(), params.hosts.end());
  }
  return Status::OK;
}

PlanNodeId Coordinator::FindLeftmostNode(
    const TPlan& plan, const std::vector<TPlanNodeType::type>& types) {
  // the first node with num_children == 0 is the leftmost node
  int node_idx = 0;
  while (node_idx < plan.nodes.size() && plan.nodes[node_idx].num_children != 0) {
    ++node_idx;
  }
  if (node_idx == plan.nodes.size()) {
    return g_JavaConstants_constants.INVALID_PLAN_NODE_ID;
  }
  const TPlanNode& node = plan.nodes[node_idx];

  for (int i = 0; i < types.size(); ++i) {
    if (node.node_type == types[i]) return node.node_id;
  }
  return g_JavaConstants_constants.INVALID_PLAN_NODE_ID;
}

int Coordinator::FindLeftmostInputFragment(
    int fragment_idx, const TQueryExecRequest& exec_request) {
  // find the leftmost node, which we expect to be an exchage node
  vector<TPlanNodeType::type> exch_node_type;
  exch_node_type.push_back(TPlanNodeType::EXCHANGE_NODE);
  PlanNodeId exch_id =
      FindLeftmostNode(exec_request.fragments[fragment_idx].plan, exch_node_type);
  if (exch_id == g_JavaConstants_constants.INVALID_PLAN_NODE_ID) {
    return g_JavaConstants_constants.INVALID_PLAN_NODE_ID;
  }

  // find the fragment that sends to this exchange node
  for (int i = 0; i < exec_request.dest_fragment_idx.size(); ++i) {
    if (exec_request.dest_fragment_idx[i] != fragment_idx) continue;
    const TPlanFragment& input_fragment = exec_request.fragments[i + 1];
    DCHECK(input_fragment.__isset.output_sink);
    DCHECK(input_fragment.output_sink.__isset.stream_sink);
    if (input_fragment.output_sink.stream_sink.dest_node_id == exch_id) return i + 1;
  }
  // this shouldn't happen
  DCHECK(false) << "no fragment sends to exch id " << exch_id;
  return g_JavaConstants_constants.INVALID_PLAN_NODE_ID;
}

void Coordinator::ComputeScanRangeAssignment(const TQueryExecRequest& exec_request) {
  // map from node id to fragment index in exec_request.fragments
  vector<PlanNodeId> per_node_fragment_idx;
  for (int i = 0; i < exec_request.fragments.size(); ++i) {
    BOOST_FOREACH(const TPlanNode& node, exec_request.fragments[i].plan.nodes) {
      if (per_node_fragment_idx.size() < node.node_id + 1) {
        per_node_fragment_idx.resize(node.node_id + 1);
      }
      per_node_fragment_idx[node.node_id] = i;
    }
  }

  scan_range_assignment_.resize(exec_request.fragments.size());
  map<TPlanNodeId, vector<TScanRangeLocations> >::const_iterator  entry;
  for (entry = exec_request.per_node_scan_ranges.begin();
      entry != exec_request.per_node_scan_ranges.end(); ++entry) {
    int fragment_idx = per_node_fragment_idx[entry->first];
    FragmentScanRangeAssignment* assignment = &scan_range_assignment_[fragment_idx];
    ComputeScanRangeAssignment(
        entry->first, entry->second, fragment_exec_params_[fragment_idx], assignment);
    num_scan_ranges_ += entry->second.size();
  }
}

int64_t GetScanRangeLength(const TScanRange& scan_range) {
  if (scan_range.__isset.hdfs_file_split) {
    return scan_range.hdfs_file_split.length;
  } else {
    return 0;
  }
}

void Coordinator::ComputeScanRangeAssignment(
    PlanNodeId node_id, const vector<TScanRangeLocations>& locations,
    const FragmentExecParams& params, FragmentScanRangeAssignment* assignment) {
  unordered_map<THostPort, int64_t> assigned_bytes_per_host;  // total assigned
  BOOST_FOREACH(const TScanRangeLocations& scan_range_locations, locations) {
    // assign this scan range to the host w/ the fewest assigned bytes
    int64_t min_assigned_bytes = numeric_limits<int64_t>::max();
    const THostPort* data_host = NULL;  // data server; not necessarily backend
    int volume_id = -1;
    BOOST_FOREACH(const TScanRangeLocation& location, scan_range_locations.locations) {
      int64_t* assigned_bytes =
          FindOrInsert(&assigned_bytes_per_host, location.server, 0L);
      if (*assigned_bytes < min_assigned_bytes) {
        min_assigned_bytes = *assigned_bytes;
        data_host = &location.server;
        volume_id = location.volume_id;
      }
    }
    assigned_bytes_per_host[*data_host] +=
        GetScanRangeLength(scan_range_locations.scan_range);

    // translate data host to backend host
    DCHECK(data_host != NULL);
    THostPort exec_hostport;
    DCHECK_GT(params.hosts.size(), 0);
    if (params.hosts.size() == 1) {
      // this is only running on the coordinator anyway
      exec_hostport = params.hosts[0];
    } else {
      FragmentExecParams::DataServerMap::const_iterator it =
          params.data_server_map.find(*data_host);
      DCHECK(it != params.data_server_map.end());
      exec_hostport = it->second;
    }
    PerNodeScanRanges* scan_ranges =
        FindOrInsert(assignment, exec_hostport, PerNodeScanRanges());
    vector<TScanRangeParams>* scan_range_params_list =
        FindOrInsert(scan_ranges, node_id, vector<TScanRangeParams>());
    // add scan range
    TScanRangeParams scan_range_params;
    scan_range_params.scan_range = scan_range_locations.scan_range;
    // Volume is is optional, so we need to set the value and the is-set bit
    scan_range_params.__set_volume_id(volume_id);
    scan_range_params_list->push_back(scan_range_params);
  }

  if (VLOG_FILE_IS_ON) {
    BOOST_FOREACH(FragmentScanRangeAssignment::value_type& entry, *assignment) {
      VLOG_FILE << "ScanRangeAssignment: server=" << ThriftDebugString(entry.first);
      BOOST_FOREACH(PerNodeScanRanges::value_type& per_node_scan_ranges, entry.second) {
        stringstream str;
        BOOST_FOREACH(TScanRangeParams& params, per_node_scan_ranges.second) {
          str << ThriftDebugString(params) << " ";
        }
        VLOG_FILE << "node_id=" << per_node_scan_ranges.first << " ranges=" << str.str();
      }
    }
  }
}

void Coordinator::SetExecPlanFragmentParams(
    int backend_num, const TPlanFragment& fragment, int fragment_idx,
    const FragmentExecParams& params, int instance_idx, const THostPort& coord,
    TExecPlanFragmentParams* rpc_params) {
  rpc_params->__set_protocol_version(ImpalaInternalServiceVersion::V1);
  rpc_params->__set_fragment(fragment);
  rpc_params->__set_desc_tbl(desc_tbl_);
  rpc_params->params.__set_query_id(query_id_);
  rpc_params->params.__set_fragment_instance_id(params.instance_ids[instance_idx]);
  THostPort exec_host = params.hosts[instance_idx];;
  PerNodeScanRanges& scan_ranges = scan_range_assignment_[fragment_idx][exec_host];
  rpc_params->params.__set_per_node_scan_ranges(scan_ranges);
  rpc_params->params.__set_per_exch_num_senders(params.per_exch_num_senders);
  rpc_params->params.__set_destinations(params.destinations);
  rpc_params->__isset.params = true;
  rpc_params->__set_coord(coord);
  rpc_params->__set_backend_num(backend_num);
  rpc_params->__set_query_globals(query_globals_);
  rpc_params->__set_query_options(query_options_);
}

}

/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 **/

#include "query_execution/PolicyEnforcerBase.hpp"

#include <cstddef>
#include <memory>
#include <queue>
#include <unordered_map>
#include <vector>

#include "catalog/CatalogDatabase.hpp"
#include "catalog/CatalogRelation.hpp"
#include "catalog/PartitionScheme.hpp"

#include "transaction/Predicate.hpp"
#include "transaction/PredicateLock.hpp"

#include "query_execution/QueryExecutionMessages.pb.h"
#include "query_execution/QueryExecutionState.hpp"
#include "query_execution/QueryExecutionTypedefs.hpp"
#include "query_execution/QueryManagerBase.hpp"
#include "relational_operators/WorkOrder.hpp"
#include "storage/StorageBlockInfo.hpp"

#include "gflags/gflags.h"
#include "glog/logging.h"

namespace quickstep {

DECLARE_bool(visualize_execution_dag);

DEFINE_bool(profile_and_report_workorder_perf, false,
    "If true, Quickstep will record the exceution time of all the individual "
    "normal work orders and report it at the end of query execution.");

PolicyEnforcerBase::PolicyEnforcerBase(CatalogDatabaseLite *catalog_database)
    : catalog_database_(catalog_database),
      profile_individual_workorders_(FLAGS_profile_and_report_workorder_perf || FLAGS_visualize_execution_dag) {
}

void PolicyEnforcerBase::processMessage(const TaggedMessage &tagged_message) {
  std::size_t query_id;
  QueryManagerBase::dag_node_index op_index;

  switch (tagged_message.message_type()) {
    case kWorkOrderCompleteMessage: {
      serialization::WorkOrderCompletionMessage proto;
      // Note: This proto message contains the time it took to execute the
      // WorkOrder. It can be accessed in this scope.
      CHECK(proto.ParseFromArray(tagged_message.message(),
                                 tagged_message.message_bytes()));
      decrementNumQueuedWorkOrders(proto);

      if (profile_individual_workorders_) {
        recordTimeForWorkOrder(proto);
      }

      query_id = proto.query_id();
      DCHECK(admitted_queries_.find(query_id) != admitted_queries_.end());

      op_index = proto.operator_index();
      admitted_queries_[query_id]->processWorkOrderCompleteMessage(op_index, proto.partition_id());
      break;
    }
    case kRebuildWorkOrderCompleteMessage: {
      serialization::WorkOrderCompletionMessage proto;
      // Note: This proto message contains the time it took to execute the
      // rebuild WorkOrder. It can be accessed in this scope.
      CHECK(proto.ParseFromArray(tagged_message.message(),
                                 tagged_message.message_bytes()));
      decrementNumQueuedWorkOrders(proto);

      query_id = proto.query_id();
      DCHECK(admitted_queries_.find(query_id) != admitted_queries_.end());

      op_index = proto.operator_index();
      admitted_queries_[query_id]->processRebuildWorkOrderCompleteMessage(op_index, proto.partition_id());
      break;
    }
    case kCatalogRelationNewBlockMessage: {
      serialization::CatalogRelationNewBlockMessage proto;
      CHECK(proto.ParseFromArray(tagged_message.message(),
                                 tagged_message.message_bytes()));

      const block_id block = proto.block_id();

      CatalogRelation *relation =
          static_cast<CatalogDatabase*>(catalog_database_)->getRelationByIdMutable(proto.relation_id());
      relation->addBlock(block);

      if (proto.has_partition_id()) {
        relation->getPartitionSchemeMutable()->addBlockToPartition(block, proto.partition_id());
      }
      return;
    }
    case kDataPipelineMessage: {
      serialization::DataPipelineMessage proto;
      CHECK(proto.ParseFromArray(tagged_message.message(),
                                 tagged_message.message_bytes()));
      query_id = proto.query_id();
      DCHECK(admitted_queries_.find(query_id) != admitted_queries_.end());

      op_index = proto.operator_index();
      admitted_queries_[query_id]->processDataPipelineMessage(
          op_index, proto.block_id(), proto.relation_id(), proto.partition_id());
      break;
    }
    case kWorkOrderFeedbackMessage: {
      WorkOrder::FeedbackMessage msg(
          const_cast<void *>(tagged_message.message()),
          tagged_message.message_bytes());
      query_id = msg.header().query_id;
      DCHECK(admitted_queries_.find(query_id) != admitted_queries_.end());

      op_index = msg.header().rel_op_index;
      admitted_queries_[query_id]->processFeedbackMessage(op_index, msg);
      break;
    }
    default:
      LOG(FATAL) << "Unknown message type found in PolicyEnforcer";
  }
  if (admitted_queries_[query_id]->queryStatus(op_index) ==
          QueryManagerBase::QueryStatusCode::kQueryExecuted) {

    auto base_ptr = admitted_queries_[query_id].get();
    const QueryHandle* completed_query = base_ptr->query_handle();
    running_queries_.erase(running_queries_.find(completed_query));

    onQueryCompletion(base_ptr);

    removeQuery(query_id);
    bool query_admitted(false);
    while ( !waiting_queries_.empty()) {
      // Admit the earliest waiting query.
      QueryHandle *new_query = waiting_queries_.front();

      bool doesIntersect = false;
      for(auto running_query : running_queries_){
        if(locks_[running_query].intersect(locks_[new_query])){
          doesIntersect = true;
          break;
        }
      }

      if(!doesIntersect){
        query_admitted=admitQuery(new_query);
        if(query_admitted) {
          LOG_WARNING("Waiting query got admitted");
          running_queries_.insert(new_query);
          waiting_queries_.pop();
        }
      }
      else{
        break;
      }
    }
  }
}

void PolicyEnforcerBase::removeQuery(const std::size_t query_id) {
  DCHECK(admitted_queries_.find(query_id) != admitted_queries_.end());
  if (!admitted_queries_[query_id]->getQueryExecutionState().hasQueryExecutionFinished()) {
    LOG(WARNING) << "Removing query with ID " << query_id
                 << " that hasn't finished its execution";
  }
  admitted_queries_.erase(query_id);
}

bool PolicyEnforcerBase::admitQueries(
    const std::vector<QueryHandle*> &query_handles) {
  DCHECK(!query_handles.empty());

  bool all_queries_admitted = true;


  //query_handle -> query_plan -> dag_operators->nodes->payload->predicate_index
  //query_handle ->query_context_proto -> predicates_

  //TODO: Move this all to a static function of PredicateLock that takes &lock_ and query_handles
  for (QueryHandle *curr_query : query_handles) {
    transaction::PredicateLock lockPredicates;

    serialization::QueryContext* query_cont=curr_query->getQueryContextProtoMutable();
    int predicateCount = query_cont->predicates_size();

    for(int i=0;i<predicateCount;i++){
      serialization::Predicate predicate = query_cont->predicates(i);

      for( std::shared_ptr<transaction::Predicate> tPred : transaction::Predicate::breakdown(predicate) ){
        lockPredicates.addPredicateRead(tPred);
      }
      //TODO: loop over selection and for every attribute touched check if we already have a predicate for that relation+attribute.  If not, add an ANY lock.
    }

    // insert the pair into the map
    locks_.insert(std::pair<QueryHandle*, transaction::PredicateLock> (curr_query,lockPredicates));

    // Only admit query if no conflicts are found
    bool doesIntersect = false;
    for(auto running_query : running_queries_){
      if(locks_[running_query].intersect(lockPredicates)){
        doesIntersect = true;
        break;
      }
    }
    if(!doesIntersect){
      bool query_admitted=admitQuery(curr_query);
      if(!query_admitted){
        all_queries_admitted = false;
        waiting_queries_.push(curr_query);
      }
      else{
        running_queries_.insert(curr_query);
        LOG_WARNING("Query Admitted!");
      }
    }
    else{
      all_queries_admitted = false;
      waiting_queries_.push(curr_query);
      LOG_WARNING("Query Conflicted!");
    }
  }

  return all_queries_admitted;
}

void PolicyEnforcerBase::recordTimeForWorkOrder(
    const serialization::WorkOrderCompletionMessage &proto) {
  const std::size_t query_id = proto.query_id();
  std::vector<WorkOrderTimeEntry> &workorder_time_entries
      = workorder_time_recorder_[query_id];
  workorder_time_entries.emplace_back();
  WorkOrderTimeEntry &entry = workorder_time_entries.back();
  entry.worker_id = proto.worker_thread_index(),
  entry.operator_id = proto.operator_index(),
  entry.start_time = proto.execution_start_time(),
  entry.end_time = proto.execution_end_time();
}

}  // namespace quickstep

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


#ifndef IMPALA_EXEC_EXEC_STATS_H
#define IMPALA_EXEC_EXEC_STATS_H

namespace impala {

// A simple container class for summary statistics gathered by a coordinator about a
// single query. We don't use counters here because a) there's a non-zero overhead
// associated with them and b) they can be compiled out; these stats are required for
// the correct operation of the query.
class ExecStats {
public:
  ExecStats(): num_rows_(0), query_type_(SELECT) {}

  int num_rows() { return num_rows_; }

  enum QueryType { SELECT = 0, INSERT };
  QueryType query_type() { return query_type_; }

  bool is_insert() { return query_type_ == INSERT; }

private:
  // Number of rows returned, or written to a table sink by this query
  int num_rows_;

  // Whether this query is an INSERT or a SELECT
  QueryType query_type_;

  // Coordinators / executors can update these stats directly, this
  // saves writing accessor methods.
  friend class Coordinator;
  friend class InProcessQueryExecutor;
  friend class ImpaladQueryExecutor;
};

}

#endif

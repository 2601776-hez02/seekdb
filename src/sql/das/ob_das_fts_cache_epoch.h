/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef OB_SQL_DAS_OB_DAS_FTS_CACHE_EPOCH_H_
#define OB_SQL_DAS_OB_DAS_FTS_CACHE_EPOCH_H_

#include <atomic>

namespace oceanbase
{
namespace sql
{
// The fulltext query result cache is keyed by this epoch: every successful
// data modification bumps it, so any write (even one that is rolled back
// later) conservatively invalidates cached fulltext query results.  Reads
// never touch it, which keeps the read path lock free.
inline std::atomic<int64_t> &fts_dml_epoch()
{
  static std::atomic<int64_t> epoch(0);
  return epoch;
}

inline void bump_fts_dml_epoch()
{
  fts_dml_epoch().fetch_add(1, std::memory_order_relaxed);
}

inline int64_t get_fts_dml_epoch()
{
  return fts_dml_epoch().load(std::memory_order_relaxed);
}

} // namespace sql
} // namespace oceanbase

#endif // OB_SQL_DAS_OB_DAS_FTS_CACHE_EPOCH_H_

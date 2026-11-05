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

#define USING_LOG_PREFIX SERVER

#include "ob_virtual_table_iterator_factory.h"
#include "share/rc/ob_server_runtime.h"
#include "observer/ob_server.h"
#include "observer/ob_server_runtime_access.h"
#include "observer/virtual_table/ob_show_table_status.h"
#include "observer/virtual_table/ob_show_tables.h"
#include "observer/virtual_table/ob_virtual_warning.h"
#include "sql/table_function/table_function.h"
#include "sql/resolver/expr/ob_raw_expr_util.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/engine/ob_physical_plan_ctx.h"
#include "observer/virtual_table/ob_session_variables.h"
#include "observer/virtual_table/ob_global_variables.h"
#include "observer/virtual_table/ob_table_columns.h"
#include "observer/virtual_table/ob_table_index.h"
#include "observer/virtual_table/ob_show_create_database.h"
#include "observer/virtual_table/ob_show_create_table.h"
#include "observer/virtual_table/ob_show_create_procedure.h"
#include "observer/virtual_table/ob_show_grants.h"
#include "observer/virtual_table/ob_information_table_privileges_table.h"
#include "observer/virtual_table/ob_information_user_privileges_table.h"
#include "observer/virtual_table/ob_information_schema_privileges_table.h"
#include "observer/virtual_table/ob_information_columns_table.h"
#include "observer/virtual_table/ob_information_parameters_table.h"
#include "observer/virtual_table/ob_information_session_variables_table.h"
#include "observer/virtual_table/ob_information_global_status_table.h"
#include "observer/virtual_table/ob_information_session_status_table.h"
#include "observer/virtual_table/ob_information_kvcache_table.h"
#include "observer/virtual_table/ob_all_concurrency_object_pool.h"
#include "observer/virtual_table/ob_mysql_user_table.h"
#include "observer/virtual_table/ob_mysql_db_table.h"
#include "observer/virtual_table/ob_mysql_proc_table.h"
#include "observer/virtual_table/ob_information_table_constraints_table.h"
#include "observer/virtual_table/ob_information_check_constraints_table.h"
#include "observer/virtual_table/ob_information_referential_constraints_table.h"
#include "observer/virtual_table/ob_information_partitions_table.h"
#include "observer/virtual_table/ob_all_disk_stat.h"
#include "observer/virtual_table/ob_all_virtual_malloc_sample_info.h"
#include "observer/virtual_table/ob_all_data_type_class_table.h"
#include "observer/virtual_table/ob_all_data_type_table.h"
#include "observer/virtual_table/ob_all_virtual_memstore_usage.h"
#include "observer/virtual_table/ob_all_virtual_tablet_info.h"
// All local virtual tables are now in a unified header
#include "observer/virtual_table/ob_all_virtual_local_tables.h"
#include "observer/virtual_table/ob_all_virtual_sys_parameter_stat.h"
#include "observer/virtual_table/ob_all_virtual_memstore_info.h"
#include "observer/virtual_table/ob_all_virtual_minor_freeze_info.h"
#include "observer/virtual_table/ob_gv_sql.h"
#include "observer/virtual_table/ob_show_database_status.h"
#include "observer/virtual_table/ob_all_virtual_tx_stat.h"
#include "observer/virtual_table/ob_all_virtual_tx_lock_stat.h"
#include "observer/virtual_table/ob_all_virtual_tx_scheduler_stat.h"
#include "observer/virtual_table/ob_all_virtual_tx_ctx_mgr_stat.h"
#include "observer/virtual_table/ob_all_virtual_engine_table.h"
#include "observer/virtual_table/ob_all_virtual_files_table.h"
#include "observer/virtual_table/ob_all_virtual_ls_info.h"
#include "observer/virtual_table/ob_plan_cache_plan_explain.h"
#include "observer/virtual_table/ob_all_virtual_ps_stat.h"
#include "observer/virtual_table/ob_all_virtual_ps_item_info.h"
#include "observer/virtual_table/ob_show_processlist.h"
#include "observer/virtual_table/ob_all_virtual_session_info.h"
#include "observer/virtual_table/ob_all_virtual_memory_info.h"
#include "observer/virtual_table/ob_all_virtual_raid_stat.h"
#include "observer/virtual_table/ob_all_virtual_tablet_sstable_macro_info.h"
#include "observer/virtual_table/ob_all_virtual_sys_task_status.h"
#include "observer/virtual_table/ob_all_virtual_macro_block_marker_status.h"
#include "observer/virtual_table/ob_all_virtual_lock_wait_stat.h"
#include "observer/virtual_table/ob_all_virtual_long_ops_status.h"
#include "observer/virtual_table/ob_all_virtual_memstore_allocator_info.h"
#include "observer/virtual_table/ob_all_virtual_server_object_pool.h"
#include "observer/virtual_table/ob_all_virtual_io_stat.h"
#include "observer/virtual_table/ob_all_virtual_bad_block_table.h"
#include "observer/virtual_table/ob_all_virtual_id_service.h"
#include "observer/virtual_table/ob_all_virtual_timestamp_service.h"
#include "rootserver/ob_local_management_service.h"
#include "observer/virtual_table/ob_virtual_core_inner_table.h"
#include "observer/virtual_table/ob_virtual_charset.h"
#include "observer/virtual_table/ob_virtual_collation.h"
#include "observer/virtual_table/ob_all_virtual_dtl_channel.h"
#include "observer/virtual_table/ob_all_virtual_dtl_memory.h"
#include "observer/virtual_table/ob_virtual_outline.h"
#include "observer/virtual_table/ob_all_virtual_sql_workarea_history_stat.h"
#include "observer/virtual_table/ob_all_virtual_sql_workarea_active.h"
#include "observer/virtual_table/ob_all_virtual_sql_workarea_histogram.h"
#include "observer/virtual_table/ob_all_virtual_sql_workarea_memory_info.h"
#include "observer/virtual_table/ob_all_virtual_table_mgr.h"
#include "observer/virtual_table/ob_all_virtual_px_worker_stat.h"
#include "observer/virtual_table/ob_all_virtual_tablet_store_stat.h"
#include "observer/virtual_table/ob_all_virtual_server_schema_info.h"
#include "observer/virtual_table/ob_all_virtual_memory_context_stat.h"
#include "observer/virtual_table/ob_all_virtual_dump_info.h"
#include "observer/virtual_table/ob_all_virtual_dag_warning_history.h"
#include "observer/virtual_table/ob_all_virtual_dag.h"
#include "observer/virtual_table/ob_all_virtual_compaction_diagnose_info.h"
#include "observer/virtual_table/ob_all_virtual_compaction_suggestion.h"
#include "observer/virtual_table/ob_all_virtual_server_compaction_progress.h"
#include "observer/virtual_table/ob_all_virtual_server_compaction_event_history.h"
#include "observer/virtual_table/ob_all_virtual_tablet_compaction_progress.h"
#include "observer/virtual_table/ob_all_virtual_tablet_stat.h"
#include "observer/virtual_table/ob_all_virtual_tablet_compaction_history.h"
#include "observer/virtual_table/ob_all_virtual_tablet_compaction_info.h"
#include "observer/virtual_table/ob_all_virtual_tablet_ddl_kv_info.h"
#include "observer/virtual_table/ob_all_virtual_ddl_sim_point_stat.h"
#include "observer/virtual_table/ob_all_virtual_tablet_pointer_status.h"
#include "observer/virtual_table/ob_all_virtual_storage_meta_memory_status.h"
#include "observer/virtual_table/ob_all_virtual_tx_data.h"
#include "observer/virtual_table/ob_all_virtual_tx_data_table.h"
#include "observer/virtual_table/ob_all_virtual_transaction_freeze_checkpoint.h"
#include "observer/virtual_table/ob_all_virtual_transaction_checkpoint.h"
#include "observer/virtual_table/ob_all_virtual_checkpoint.h"
#include "observer/virtual_table/ob_virtual_open_cursor_table.h"
#include "observer/virtual_table/ob_all_virtual_ctx_memory_info.h"
#include "observer/virtual_table/ob_all_virtual_io_status.h"
#include "observer/virtual_table/ob_information_triggers_table.h"
#include "observer/virtual_table/ob_show_create_trigger.h"
#include "observer/virtual_table/ob_all_virtual_px_target_monitor.h"
#include "observer/virtual_table/ob_all_virtual_load_data_stat.h"
#include "observer/virtual_table/ob_all_virtual_dtl_interm_result_monitor.h"
#include "observer/virtual_table/ob_all_virtual_log_stat.h"
#include "observer/virtual_table/ob_all_virtual_apply_stat.h"
#include "observer/virtual_table/ob_all_virtual_replay_stat.h"
#include "observer/virtual_table/ob_all_virtual_server.h"
#include "observer/virtual_table/ob_all_virtual_obj_lock.h"
#include "observer/virtual_table/ob_virtual_privilege.h"
#include "observer/virtual_table/ob_all_virtual_kvcache_store_memblock.h"
#include "observer/virtual_table/ob_all_virtual_schema_memory.h"
#include "observer/virtual_table/ob_all_virtual_schema_slot.h"
#include "observer/virtual_table/ob_virtual_show_trace.h"
#include "observer/virtual_table/ob_all_virtual_sql_plan.h"
#include "observer/virtual_table/ob_all_virtual_mds_node_stat.h"
#include "observer/virtual_table/ob_all_virtual_mds_event_history.h"
#include "observer/virtual_table/ob_all_virtual_opt_stat_gather_monitor.h"
#include "observer/virtual_table/ob_all_virtual_px_p2p_datahub.h"
#include "observer/virtual_table/ob_all_virtual_tablet_buffer_info.h"
#include "observer/virtual_table/ob_all_virtual_activity_metrics.h"
#include "observer/virtual_table/ob_all_virtual_resource_limit.h"
#include "observer/virtual_table/ob_all_virtual_resource_limit_detail.h"
#include "observer/virtual_table/ob_all_virtual_tracepoint_info.h"
#include "observer/virtual_table/ob_all_virtual_nic_info.h"
#include "observer/virtual_table/ob_all_virtual_sys_variable_default_value.h"
#include "observer/virtual_table/ob_all_virtual_session_ps_info.h"
#include "observer/virtual_table/ob_information_schema_enable_roles_table.h"
#include "observer/virtual_table/ob_all_virtual_scheduler_running_job.h"
#include "observer/virtual_table/ob_all_virtual_dml_stats.h"
#include "observer/virtual_table/ob_all_virtual_sql_stat.h"
#include "observer/virtual_table/ob_all_virtual_vector_index_info.h"
#include "observer/virtual_table/ob_all_virtual_tmp_file.h"
#include "observer/virtual_table/ob_all_virtual_dml_stats.h"
#include "observer/virtual_table/ob_all_virtual_change_stream_refresh_stat.h"
#include "observer/virtual_table/ob_all_virtual_vector_mem_info.h"

namespace oceanbase
{
using namespace common;
using namespace share;
using namespace share::schema;
using namespace sql;
using namespace rootserver;
using namespace transaction;
namespace observer
{
#define NEW_VIRTUAL_TABLE(virtual_table, vt, ...) \
  ({ \
    void *storage = allocator.alloc(sizeof(virtual_table)); \
    vt = nullptr; \
    if (OB_ISNULL(storage)) { \
      ret = OB_ALLOCATE_MEMORY_FAILED; \
    } else { \
      vt = new (storage) virtual_table(__VA_ARGS__); \
      vt_iter = static_cast<ObVirtualTableIterator *>(vt); \
      if (OB_FAIL(vt_iter->set_key_ranges(key_ranges))) { \
        vt_iter->~ObVirtualTableIterator(); \
        allocator.free(storage); \
        vt_iter = nullptr; \
      } else { \
        vt_iter->set_session(session); \
        vt_iter->set_schema_guard(&schema_guard); \
        vt_iter->set_function_definition(&definition); \
        vt_iter->set_scan_flag(scan_flag); \
      } \
    } \
    ret; \
  })

#define BEGIN_CREATE_VT_ITER_SWITCH_LAMBDA                                              \
        [&]() {                                                                         \
          if (!processed) {                                                             \
            processed = true;                                                           \
            switch (pure_tid) {

#define END_CREATE_VT_ITER_SWITCH_LAMBDA                                                \
              default: {                                                                \
                processed = false;                                                      \
                break;                                                                  \
              }                                                                         \
            }                                                                           \
          }                                                                             \
        }();


ObVirtualTableIteratorFactory::ObVirtualTableIteratorFactory(ObVTIterCreator &vt_iter_creator) :
    ObIVirtualTableIteratorFactory(),
    vt_iter_creator_(vt_iter_creator)
{
}

ObVirtualTableIteratorFactory::ObVirtualTableIteratorFactory(ObLocalManagementService &local_management_service,
                                                             common::ObAddr &addr) :
    ObIVirtualTableIteratorFactory(),
    vt_iter_creator_(local_management_service, addr)
{
}

ObVirtualTableIteratorFactory::~ObVirtualTableIteratorFactory()
{
}

int ObVirtualTableIteratorFactory::create_virtual_table_iterator(ObVTableScanParam &params,
                                                                 ObVirtualTableIterator *&vt_iter)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(vt_iter_creator_.create_vt_iter(params, vt_iter))) {
  }
  return ret;
}

int ObVirtualTableIteratorFactory::revert_virtual_table_iterator(ObVirtualTableIterator *vt_iter)
{
int ret = OB_SUCCESS;
  if (OB_UNLIKELY(NULL == vt_iter)) {
    ret = OB_ERR_UNEXPECTED;
    SERVER_LOG(WARN, "vt_iter is NULL, can not free it");
  } else {
    vt_iter->~ObVirtualTableIterator();
    vt_iter = NULL;
  }
  return ret;
}

int ObVirtualTableIteratorFactory::check_can_create_iter(common::ObVTableScanParam &params)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(vt_iter_creator_.check_can_create_iter(params))) {
  }
  return ret;
}

int ObVTIterCreator::create_vt_iter(ObVTableScanParam &params,
                                    ObVirtualTableIterator *&vt_iter)
{
  int ret = OB_SUCCESS;
  const TableFunctionDescriptor *definition = TableFunctionRegistry::find(params.index_id_);
  ObSchemaGetterGuard &schema_guard = params.get_schema_guard();
  if (OB_ISNULL(definition) || OB_ISNULL(params.op_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(local_management_service_.get_schema_service().get_runtime_schema_guard(schema_guard))) {
  } else if (OB_FAIL(create_function_iter(*definition, *params.scan_allocator_,
                 params.op_->get_eval_ctx().exec_ctx_.get_my_session(), schema_guard,
                 params.key_ranges_, params.scan_flag_, vt_iter))) {
  } else {
    vt_iter->set_timeout(params.timeout_);
  }
  return ret;
}

int ObVTIterCreator::create_function_iter(const TableFunctionDescriptor &definition,
                                         ObIAllocator &allocator,
                                         ObSQLSessionInfo *session,
                                         ObSchemaGetterGuard &schema_guard,
                                         const ObIArray<ObNewRange> &key_ranges,
                                         const ObQueryFlag &scan_flag,
                                         ObVirtualTableIterator *&vt_iter)
{
  int ret = OB_SUCCESS;
  const uint64_t pure_tid = definition.id_;
  vt_iter = nullptr;
  const int simulate_error = EVENT_CALL(EventTable::EN_DAS_SIMULATE_VT_CREATE_ERROR);
  if (OB_UNLIKELY(OB_SUCCESS != simulate_error)) {
    ret = simulate_error;
  } else if (OB_ISNULL(session) || OB_ISNULL(get_observer_sql_engine())
             || OB_ISNULL(GCTX.schema_service_) || OB_ISNULL(GCTX.sql_proxy_)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
        bool processed = false;

        BEGIN_CREATE_VT_ITER_SWITCH_LAMBDA


          case OB_ALL_VIRTUAL_PRIVILEGE_TID: {
            ObVirtualPrivilege *privilege_iter = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObVirtualPrivilege, privilege_iter))) {
            } else {
              vt_iter = static_cast<ObVirtualTableIterator *>(privilege_iter);
            }
            break;
          }

          case OB_ALL_VIRTUAL_ALL_TABLE_TID: {
            ObShowTableStatus *table_status = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObShowTableStatus, table_status))) {
            } else {
              table_status->set_allocator(&allocator);
              
              table_status->set_sql_proxy(GCTX.sql_proxy_);
              vt_iter = static_cast<ObVirtualTableIterator *>(table_status);
            }
            break;
          }
          case OB_ALL_VIRTUAL_SHOW_TABLES_TID: {
            ObShowTables *show_tables = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObShowTables, show_tables))) {
            } else {
              show_tables->set_allocator(&allocator);
              
              vt_iter = static_cast<ObVirtualTableIterator *>(show_tables);
            }
            break;
          }
          case OB_ALL_VIRTUAL_CORE_ALL_TABLE_TID: {
            ObVritualCoreInnerTable *core_all_table = NULL;
            const char *table_name = NULL;
            if (OB_FAIL(ObSchemaUtils::get_all_table_history_name(table_name))) {
            } else if (OB_FAIL(NEW_VIRTUAL_TABLE(ObVritualCoreInnerTable, core_all_table))) {
            } else if (OB_FAIL(core_all_table->init(*GCTX.sql_proxy_,
                                                    table_name,
                                                    pure_tid,
                                                    &schema_guard))) {
            } else {
              vt_iter = static_cast<ObVirtualTableIterator *>(core_all_table);
            }
            break;
          }
          case OB_ALL_VIRTUAL_CORE_COLUMN_TABLE_TID: {
            ObVritualCoreInnerTable *core_column_table = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObVritualCoreInnerTable, core_column_table))) {
            } else if (OB_FAIL(core_column_table->init(*GCTX.sql_proxy_,
                                                       OB_ALL_COLUMN_HISTORY_TNAME,
                                                       pure_tid,
                                                       &schema_guard))) {
            } else {
              vt_iter = static_cast<ObVirtualTableIterator *>(core_column_table);
            }
            break;
          }
        END_CREATE_VT_ITER_SWITCH_LAMBDA

        BEGIN_CREATE_VT_ITER_SWITCH_LAMBDA
          case OB_ALL_VIRTUAL_MEMSTORE_INFO_TID: {
            ObAllVirtualMemstoreUsage *memstore_usage = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObAllVirtualMemstoreUsage, memstore_usage))) {
            } else {
              vt_iter = static_cast<ObVirtualTableIterator *>(memstore_usage);
            }
            break;
          }
          case OB_ALL_VIRTUAL_MEMORY_INFO_TID: {
            ObAllVirtualMemoryInfo *all_virtual_memory_info = NULL;
            if (OB_SUCC(NEW_VIRTUAL_TABLE(ObAllVirtualMemoryInfo, all_virtual_memory_info))) {
              all_virtual_memory_info->set_allocator(&allocator);
              vt_iter = static_cast<ObVirtualTableIterator *>(all_virtual_memory_info);
            }
            break;
          }
          case OB_ALL_VIRTUAL_PARAMETER_STAT_TID:
          case OB_ALL_VIRTUAL_SYS_PARAMETER_STAT_TID: {
            ObAllVirtualSysParameterStat *all_virtual_sys_parameter_stat = NULL;
            if (OB_SUCC(NEW_VIRTUAL_TABLE(ObAllVirtualSysParameterStat,
                                          all_virtual_sys_parameter_stat))) {
              vt_iter = static_cast<ObAllVirtualSysParameterStat *>(all_virtual_sys_parameter_stat);
            }
            break;
          }
          case OB_ALL_VIRTUAL_TABLET_MEMSTORE_INFO_TID: {
            ObAllVirtualMemstoreInfo *all_virtual_memstore_info = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObAllVirtualMemstoreInfo, all_virtual_memstore_info))) {
            } else {
              vt_iter = static_cast<ObVirtualTableIterator *>(all_virtual_memstore_info);
            }
            break;
          }
          case OB_ALL_VIRTUAL_MINOR_FREEZE_INFO_TID: {
            ObAllVirtualMinorFreezeInfo *all_virtual_minor_freeze_info = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObAllVirtualMinorFreezeInfo, all_virtual_minor_freeze_info))) {
            } else {
              vt_iter = static_cast<ObVirtualTableIterator *>(all_virtual_minor_freeze_info);
            }
            break;
          }
          case OB_ALL_VIRTUAL_LS_INFO_TID: {
            ObAllVirtualLSInfo *all_virtual_ls_info = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObAllVirtualLSInfo, all_virtual_ls_info))) {
            } else {
              vt_iter = static_cast<ObVirtualTableIterator *>(all_virtual_ls_info);
            }
            break;
          }
          case OB_ALL_VIRTUAL_OBJ_LOCK_TID: {
            ObAllVirtualObjLock *all_virtual_obj_lock = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObAllVirtualObjLock, all_virtual_obj_lock))) {
            } else {
              vt_iter = static_cast<ObVirtualTableIterator *>(all_virtual_obj_lock);
            }
            break;
          }
          case OB_ALL_VIRTUAL_TABLET_INFO_TID: {
            ObAllVirtualTabletInfo *all_virtual_tablet_info = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObAllVirtualTabletInfo, all_virtual_tablet_info))) {
            } else {
              vt_iter = static_cast<ObVirtualTableIterator *>(all_virtual_tablet_info);
            }
            break;
          }
          case OB_ALL_VIRTUAL_TX_DATA_TID: {
            ObAllVirtualTxData *all_virtual_tx_data = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObAllVirtualTxData, all_virtual_tx_data))) {
            } else {
              vt_iter = static_cast<ObVirtualTableIterator *>(all_virtual_tx_data);
            }
            break;
          }
          case OB_ALL_VIRTUAL_TX_DATA_TABLE_TID: {
            ObAllVirtualTxDataTable *all_virtual_tx_data_table = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObAllVirtualTxDataTable, all_virtual_tx_data_table))) {
            } else {
              vt_iter = static_cast<ObVirtualTableIterator *>(all_virtual_tx_data_table);
            }
            break;
          }
          case OB_ALL_VIRTUAL_TRANSACTION_FREEZE_CHECKPOINT_TID: {
            ObAllVirtualFreezeCheckpointInfo *all_virtual_transaction_freeze_checkpoint = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObAllVirtualFreezeCheckpointInfo,
                  all_virtual_transaction_freeze_checkpoint))) {
            } else {
              vt_iter = static_cast<ObVirtualTableIterator *>(all_virtual_transaction_freeze_checkpoint);
            }
            break;
          }
          case OB_ALL_VIRTUAL_TRANSACTION_CHECKPOINT_TID: {
            ObAllVirtualTransCheckpointInfo *all_virtual_trans_checkpoint = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObAllVirtualTransCheckpointInfo,
                  all_virtual_trans_checkpoint))) {
            } else {
              vt_iter = static_cast<ObVirtualTableIterator *>(all_virtual_trans_checkpoint);
            }
            break;
          }
          case OB_ALL_VIRTUAL_CHECKPOINT_TID: {
            ObAllVirtualCheckpointInfo *all_virtual_checkpoint = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObAllVirtualCheckpointInfo,
                  all_virtual_checkpoint))) {
            } else {
              vt_iter = static_cast<ObVirtualTableIterator *>(all_virtual_checkpoint);
            }
            break;
          }
          case OB_ALL_VIRTUAL_TABLE_MGR_TID: {
            ObAllVirtualTableMgr *table_mgr = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObAllVirtualTableMgr, table_mgr))) {
            } else if (OB_FAIL(table_mgr->init(&allocator))) {
            } else {
              vt_iter = static_cast<ObVirtualTableIterator *>(table_mgr);
            }
            break;
          }
          case OB_ALL_VIRTUAL_STORAGE_META_MEMORY_STATUS_TID: {
            ObAllVirtualStorageMetaMemoryStatus *mem_status = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObAllVirtualStorageMetaMemoryStatus, mem_status))) {
            } else if (OB_FAIL(mem_status->init(&allocator))) {
            } else {
              vt_iter = static_cast<ObVirtualTableIterator *>(mem_status);
            }
            break;
          }
          case OB_ALL_VIRTUAL_TABLET_POINTER_STATUS_TID: {
            ObAllVirtualTabletPtr *tablet_ptr_status = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObAllVirtualTabletPtr, tablet_ptr_status))) {
            } else if (OB_FAIL(tablet_ptr_status->init(&allocator))) {
            } else {
              vt_iter = static_cast<ObVirtualTableIterator *>(tablet_ptr_status);
            }
            break;
          }
          case OB_ALL_VIRTUAL_RAID_STAT_TID: {
            ObAllVirtualRaidStat *raid_stat = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObAllVirtualRaidStat, raid_stat))) {
            } else if (OB_FAIL(raid_stat->init(addr_))) {
            } else {
              vt_iter = static_cast<ObVirtualTableIterator *>(raid_stat);
            }
            break;
          }
          case OB_ALL_VIRTUAL_TABLET_DDL_KV_INFO_TID: {
            ObAllVirtualTabletDDLKVInfo *ddl_kv_info = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObAllVirtualTabletDDLKVInfo, ddl_kv_info))) {
            } else {
              vt_iter = static_cast<ObVirtualTableIterator *>(ddl_kv_info);
            }
            break;
          }
        END_CREATE_VT_ITER_SWITCH_LAMBDA

        BEGIN_CREATE_VT_ITER_SWITCH_LAMBDA
          case OB_ALL_VIRTUAL_TRANS_STAT_TID: {
            ObGVTxStat *gv_tx_stat = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObGVTxStat, gv_tx_stat))) {
            } else {
              vt_iter = static_cast<ObVirtualTableIterator *>(gv_tx_stat);
            }
            break;
          }
          case OB_ALL_VIRTUAL_TRANS_SCHEDULER_TID: {
            ObGVTxSchedulerStat *gv_tx_scheduler_stat = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObGVTxSchedulerStat, gv_tx_scheduler_stat))) {
            } else {
              vt_iter = static_cast<ObVirtualTableIterator *>(gv_tx_scheduler_stat);
            }
            break;
          }
          case OB_ALL_VIRTUAL_TRANS_LOCK_STAT_TID: {
            ObGVTxLockStat *gv_tx_lock_stat = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObGVTxLockStat, gv_tx_lock_stat))) {
            } else {
              vt_iter = static_cast<ObVirtualTableIterator *>(gv_tx_lock_stat);
            }
            break;
          }
          case OB_ALL_VIRTUAL_TRANS_CTX_MGR_STAT_TID: {
            ObGVTxCtxMgrStat *gv_tx_ctx_mgr_stat = NULL;
            transaction::ObTransService *txs = ::oceanbase::share::server_service<::oceanbase::transaction::ObTransService>();
            if (OB_UNLIKELY(NULL == txs)) {
              SERVER_LOG(WARN, "invalid argument", KP(txs));
              ret = OB_INVALID_ARGUMENT;
            } else if (OB_FAIL(NEW_VIRTUAL_TABLE(ObGVTxCtxMgrStat,
                                                 gv_tx_ctx_mgr_stat, txs))) {
            } else {
              vt_iter = static_cast<ObVirtualTableIterator *>(gv_tx_ctx_mgr_stat);
            }
            break;
          }
          case OB_ALL_VIRTUAL_PLAN_CACHE_STAT_TID: {
              ObAllPlanCacheBase *pcs = NULL;
              OZ(NEW_VIRTUAL_TABLE(ObAllPlanCacheStat, pcs));
              if (OB_SUCC(ret)) {
                vt_iter = static_cast<ObVirtualTableIterator *>(pcs);
              }
            } break;
          case OB_ALL_VIRTUAL_PLAN_STAT_TID: {
            ObAllPlanCacheBase *pcs = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObGVSql, pcs))) {
            } else {
              vt_iter = static_cast<ObVirtualTableIterator *>(pcs);
            }
          } break;
          case OB_ALL_VIRTUAL_PLAN_CACHE_PLAN_EXPLAIN_TID: {
            ObPlanCachePlanExplain *px = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObPlanCachePlanExplain, px))) {
            } else {
              px->set_allocator(&allocator);
              vt_iter = static_cast<ObVirtualTableIterator *>(px);
            }
          } break;
          case OB_ALL_VIRTUAL_PS_STAT_TID: {
            ObAllVirtualPsStat *ps_stat = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObAllVirtualPsStat, ps_stat))) {
            } else if (OB_ISNULL(ps_stat)) {
              ret = OB_ERR_UNEXPECTED;
              SERVER_LOG(WARN, "ps_stat init failed", K(ret));
            } else {
              // init code
              vt_iter = static_cast<ObAllVirtualPsStat *>(ps_stat);
            }
            break;
          }
          case OB_ALL_VIRTUAL_PS_ITEM_INFO_TID: {
            ObAllVirtualPsItemInfo *ps_item_info = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObAllVirtualPsItemInfo, ps_item_info))) {
            } else if (OB_ISNULL(ps_item_info)) {
              ret = OB_ERR_UNEXPECTED;
              SERVER_LOG(WARN, "ps_item_info init failed", K(ret));
            } else {
              // init code
              vt_iter = static_cast<ObAllVirtualPsItemInfo *>(ps_item_info);
            }
            break;
          }
          case OB_ALL_VIRTUAL_SESSION_PS_INFO_TID: {
            ObAllVirtualSessionPsInfo *session_ps_info = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObAllVirtualSessionPsInfo, session_ps_info))) {
            } else if (OB_ISNULL(session_ps_info)) {
              ret = OB_ERR_UNEXPECTED;
              SERVER_LOG(WARN, "session_ps_info init failed", K(ret));
            } else {
              // init code
              vt_iter = static_cast<ObAllVirtualSessionPsInfo *>(session_ps_info);
            }
            break;
          }
        END_CREATE_VT_ITER_SWITCH_LAMBDA

        BEGIN_CREATE_VT_ITER_SWITCH_LAMBDA
          case OB_ALL_VIRTUAL_SESSION_VARIABLE_TID:
          {
            ObSessionVariables *session_variables = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObSessionVariables, session_variables))) {
            } else {
              const ObSysVariableSchema *sys_variable_schema = NULL;
              if (OB_FAIL(schema_guard.get_sys_variable_schema( sys_variable_schema))) {
              } else if (OB_ISNULL(sys_variable_schema)) {
                ret = OB_ERR_UNEXPECTED;
                SERVER_LOG(WARN, "sys variable schema is null", K(ret));
              } else {
                session_variables->set_sys_variable_schema(sys_variable_schema);
                vt_iter = static_cast<ObVirtualTableIterator *>(session_variables);
              }
            }
            break;
          }
          case OB_ALL_VIRTUAL_GLOBAL_VARIABLE_TID:
          {
            ObGlobalVariables *global_variables = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObGlobalVariables, global_variables))) {
            } else {
              global_variables->set_sql_proxy(GCTX.sql_proxy_);
              const ObSysVariableSchema *sys_variable_schema = NULL;
              if (OB_FAIL(schema_guard.get_sys_variable_schema( sys_variable_schema))) {
              } else if (OB_ISNULL(sys_variable_schema)) {
                ret = OB_ERR_UNEXPECTED;
                SERVER_LOG(WARN, "sys variable schema is null", K(ret));
              } else {
                global_variables->set_sys_variable_schema(sys_variable_schema);
                vt_iter = static_cast<ObVirtualTableIterator *>(global_variables);
              }
            }
            break;
          }
          case OB_ALL_VIRTUAL_TABLE_COLUMN_TID:
          {
            ObTableColumns *table_columns = NULL;
            if (OB_SUCC(NEW_VIRTUAL_TABLE(ObTableColumns, table_columns))) {
              if (OB_FAIL(table_columns->init())) {
              } else {
                vt_iter = static_cast<ObVirtualTableIterator *>(table_columns);
              }
            }
            break;
          }
          case OB_ALL_VIRTUAL_TABLE_INDEX_TID: {
            ObTableIndex *table_index = NULL;
            if (OB_SUCC(NEW_VIRTUAL_TABLE(ObTableIndex, table_index))) {
              if (OB_FAIL(table_index->init())) {
              } else {
                vt_iter = static_cast<ObVirtualTableIterator *>(table_index);
              }
            }
            break;
          }
          case OB_ALL_VIRTUAL_SHOW_CREATE_DATABASE_TID: {
            ObShowCreateDatabase *create_database = NULL;
            if (OB_SUCC(NEW_VIRTUAL_TABLE(ObShowCreateDatabase, create_database))) {
              vt_iter = static_cast<ObVirtualTableIterator *>(create_database);
            }
            break;
          }
          case OB_ALL_VIRTUAL_SHOW_CREATE_TABLE_TID:
          {
            ObShowCreateTable *create_table = NULL;
            if (OB_SUCC(NEW_VIRTUAL_TABLE(ObShowCreateTable, create_table))) {
              vt_iter = static_cast<ObVirtualTableIterator *>(create_table);
            }
            break;
          }
          case OB_ALL_VIRTUAL_SHOW_CREATE_PROCEDURE_TID:
          {
            ObShowCreateProcedure *create_proc = NULL;
            if (OB_SUCC(NEW_VIRTUAL_TABLE(ObShowCreateProcedure, create_proc))) {
              if (OB_FAIL(session->get_session_priv_info(create_proc->get_session_priv()))) {
              } else if (OB_FAIL(create_proc->get_role_id_array().assign(session->get_enable_role_array()))) {
              } else {
                vt_iter = static_cast<ObVirtualTableIterator *>(create_proc);
              }
            }
            break;
          }
          case OB_ALL_VIRTUAL_SHOW_CREATE_TRIGGER_TID:
          {
              ObShowCreateTrigger *create_tg = NULL;
            if (OB_SUCC(NEW_VIRTUAL_TABLE(ObShowCreateTrigger, create_tg))) {
              vt_iter = static_cast<ObVirtualTableIterator *>(create_tg);
            }
            break;
          }
          case OB_ALL_VIRTUAL_PRIVILEGE_GRANT_TID:
          {
            ObShowGrants *show_grants = NULL;
            if (OB_SUCC(NEW_VIRTUAL_TABLE(ObShowGrants, show_grants))) {
              
              show_grants->set_user_id(session->get_user_id());
              if (OB_FAIL(session->get_session_priv_info(show_grants->get_session_priv()))) {
              } else if (OB_FAIL(show_grants->get_role_id_array().assign(session->get_enable_role_array()))) {
              } else {
                vt_iter = static_cast<ObVirtualTableIterator *>(show_grants);
              }
            }
            break;
          }
          case OB_SESSION_VARIABLES_TID: {
            ObInfoSchemaSessionVariablesTable *session_variables = NULL;
            if (OB_SUCC(NEW_VIRTUAL_TABLE(ObInfoSchemaSessionVariablesTable,
                                          session_variables))) {
              const ObSysVariableSchema *sys_variable_schema = NULL;
              if (OB_FAIL(schema_guard.get_sys_variable_schema( sys_variable_schema))) {
              } else if (OB_ISNULL(sys_variable_schema)) {
                ret = OB_ERR_UNEXPECTED;
                SERVER_LOG(WARN, "sys variable schema is null", K(ret));
              } else {
                session_variables->set_sys_variable_schema(sys_variable_schema);
                vt_iter = static_cast<ObVirtualTableIterator *>(session_variables);
              }
            }
            break;
          }
          case OB_GLOBAL_STATUS_TID: {
            ObInfoSchemaGlobalStatusTable *global_status = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObInfoSchemaGlobalStatusTable, global_status))) {
            } else {
              global_status->set_cur_session(session);
              global_status->set_global_ctx(&GCTX);
              vt_iter = static_cast<ObVirtualTableIterator *>(global_status);
            }
            break;
          }
          case OB_SESSION_STATUS_TID: {
            ObInfoSchemaSessionStatusTable *session_status = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObInfoSchemaSessionStatusTable, session_status))) {
            } else {
              session_status->set_cur_session(session);
              session_status->set_global_ctx(&GCTX);
              vt_iter = static_cast<ObVirtualTableIterator *>(session_status);
            }
            break;
          }



        END_CREATE_VT_ITER_SWITCH_LAMBDA

        BEGIN_CREATE_VT_ITER_SWITCH_LAMBDA
          case OB_ALL_VIRTUAL_INFORMATION_COLUMNS_TID: {
            ObInfoSchemaColumnsTable *columns = NULL;
            if (OB_SUCC(NEW_VIRTUAL_TABLE(ObInfoSchemaColumnsTable, columns))) {
              
              vt_iter = static_cast<ObVirtualTableIterator *>(columns);
            }
            break;
          }
          case OB_ALL_VIRTUAL_PROCESSLIST_TID:
          {
            ObShowProcesslist *processlist_show = NULL;
            if (OB_SUCC(NEW_VIRTUAL_TABLE(ObShowProcesslist, processlist_show))) {
              processlist_show->sesession_pool(::oceanbase::share::server_service<::oceanbase::sql::ObSQLSessionMgr>());
              vt_iter = static_cast<ObVirtualTableIterator *>(processlist_show);
            }
            break;
          }
          case OB_ALL_VIRTUAL_SESSION_INFO_TID:
          {
            ObAllVirtualSessionInfo *session_info = NULL;
            if (OB_SUCC(NEW_VIRTUAL_TABLE(ObAllVirtualSessionInfo, session_info))) {
              session_info->sesession_pool(::oceanbase::share::server_service<::oceanbase::sql::ObSQLSessionMgr>());
              vt_iter = static_cast<ObVirtualTableIterator *>(session_info);
            }
            break;
          }
          case OB_ALL_VIRTUAL_DATABASE_STATUS_TID: {
            ObShowDatabaseStatus *database_status = NULL;
            if (OB_SUCC(NEW_VIRTUAL_TABLE(ObShowDatabaseStatus, database_status))) {
              
              vt_iter = static_cast<ObVirtualTableIterator *>(database_status);
            }
            break;
          }
          case OB_USER_TID: {
            ObMySQLUserTable *mysql_user_table = NULL;
            if (OB_SUCC(NEW_VIRTUAL_TABLE(ObMySQLUserTable, mysql_user_table))) {
              
              vt_iter = static_cast<ObVirtualTableIterator *>(mysql_user_table);
            }
            break;
          }
          case OB_DB_TID: {
            ObMySQLDBTable *mysql_db_table = NULL;
            if (OB_SUCC(NEW_VIRTUAL_TABLE(ObMySQLDBTable, mysql_db_table))) {
              
              vt_iter = static_cast<ObVirtualTableIterator *>(mysql_db_table);
            }
            break;
          }




          case OB_ALL_VIRTUAL_KVCACHE_INFO_TID: {
            ObInfoSchemaKvCacheTable *cache_table = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObInfoSchemaKvCacheTable, cache_table))) {
            } else {
              cache_table->set_addr(addr_);
              vt_iter = static_cast<ObVirtualTableIterator *>(cache_table);
            }
            break;
          }
          case OB_ALL_VIRTUAL_CONCURRENCY_OBJECT_POOL_TID: {
            ObAllConcurrencyObjectPool *object_pool = NULL;
            if (OB_SUCC(NEW_VIRTUAL_TABLE(ObAllConcurrencyObjectPool, object_pool))) {
              vt_iter = static_cast<ObVirtualTableIterator *>(object_pool);
            }
            break;
          }
          case OB_ALL_VIRTUAL_MALLOC_SAMPLE_INFO_TID: {
            ObMallocSampleInfo *malloc_sample_info = NULL;
            if (OB_SUCC(NEW_VIRTUAL_TABLE(ObMallocSampleInfo, malloc_sample_info))) {
              malloc_sample_info->set_allocator(&allocator);
              vt_iter = static_cast<ObVirtualTableIterator *>(malloc_sample_info);
            }
            break;
          }
          case OB_ALL_VIRTUAL_WARNING_TID: {
            ObVirtualWarning *warning = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObVirtualWarning,
                                          warning))) {
            } else {
              vt_iter = static_cast<ObVirtualTableIterator *>(warning);
            }
            break;
          }
          case OB_ALL_VIRTUAL_SHOW_TRACE_TID:
          {
            ObVirtualShowTrace *show_trace = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObVirtualShowTrace,
                                          show_trace))) {
            } else {
              vt_iter = static_cast<ObVirtualTableIterator *>(show_trace);
            }
            break;
          }
          case OB_ALL_VIRTUAL_DATA_TYPE_CLASS_TID: {
            ObAllDataTypeClassTable *all_tc_table = NULL;
            if (OB_SUCC(NEW_VIRTUAL_TABLE(ObAllDataTypeClassTable, all_tc_table))) {
              all_tc_table->set_allocator(&allocator);
              vt_iter = static_cast<ObVirtualTableIterator *>(all_tc_table);
            }
            break;
          }
        END_CREATE_VT_ITER_SWITCH_LAMBDA

        BEGIN_CREATE_VT_ITER_SWITCH_LAMBDA
          case OB_ALL_VIRTUAL_DATA_TYPE_TID: {
            ObAllDataTypeTable *all_type_table = NULL;
            if (OB_SUCC(NEW_VIRTUAL_TABLE(ObAllDataTypeTable, all_type_table))) {
              all_type_table->set_allocator(&allocator);
              vt_iter = static_cast<ObVirtualTableIterator *>(all_type_table);
            }
            break;
          }
          case OB_ALL_VIRTUAL_DISK_STAT_TID: {
            ObInfoSchemaDiskStatTable *disk_stat_table = NULL;
            if (OB_SUCC(NEW_VIRTUAL_TABLE(ObInfoSchemaDiskStatTable, disk_stat_table))) {
              disk_stat_table->set_addr(addr_);
              vt_iter = static_cast<ObVirtualTableIterator *>(disk_stat_table);
            }
            break;
          }
          case OB_ALL_VIRTUAL_SERVER_OBJECT_POOL_TID: {
            ObAllVirtualServerObjectPool *server_object_pool = NULL;
            if (OB_SUCC(NEW_VIRTUAL_TABLE(ObAllVirtualServerObjectPool, server_object_pool))) {
              vt_iter = static_cast<ObVirtualTableIterator *>(server_object_pool);
            }
            break;
          }
          case OB_ALL_VIRTUAL_ENGINE_TID: {
            ObAllVirtualEngineTable *all_engines_table = NULL;
            if (OB_SUCC(NEW_VIRTUAL_TABLE(ObAllVirtualEngineTable, all_engines_table))) {
              all_engines_table->set_allocator(&allocator);
              vt_iter = static_cast<ObVirtualTableIterator *>(all_engines_table);
            }
            break;
          }
          case OB_ALL_VIRTUAL_LOG_STAT_TID: {
            ObAllVirtualPalfStat *palf_stat = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObAllVirtualPalfStat, palf_stat))) {
            } else {
              vt_iter = static_cast<ObVirtualTableIterator *>(palf_stat);
            }
            break;
          }
          case OB_ALL_VIRTUAL_APPLY_STAT_TID: {
            ObAllVirtualApplyStat *apply_stat = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObAllVirtualApplyStat, apply_stat))) {
            } else {
              vt_iter = static_cast<ObAllVirtualApplyStat *>(apply_stat);
            }
            break;
          }
          case OB_ALL_VIRTUAL_REPLAY_STAT_TID: {
            ObAllVirtualReplayStat *replay_stat = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObAllVirtualReplayStat, replay_stat))) {
            } else {
              vt_iter = static_cast<ObAllVirtualReplayStat *>(replay_stat);
            }
            break;
          }
        END_CREATE_VT_ITER_SWITCH_LAMBDA

        BEGIN_CREATE_VT_ITER_SWITCH_LAMBDA
          case OB_ALL_VIRTUAL_FILES_TID: {
            ObAllVirtualFilesTable *all_files_table = NULL;
            if (OB_SUCC(NEW_VIRTUAL_TABLE(ObAllVirtualFilesTable, all_files_table))) {
              all_files_table->set_allocator(&allocator);
              vt_iter = static_cast<ObVirtualTableIterator *>(all_files_table);
            }
            break;
          }
          case OB_ALL_VIRTUAL_TABLET_SSTABLE_MACRO_INFO_TID: {
            ObAllVirtualTabletSSTableMacroInfo *sstable_macro_info = NULL;
            if (OB_SUCC(NEW_VIRTUAL_TABLE(ObAllVirtualTabletSSTableMacroInfo, sstable_macro_info))) {
              if (OB_FAIL(sstable_macro_info->init(&allocator))) {
              } else {
                vt_iter = static_cast<ObVirtualTableIterator *>(sstable_macro_info);
              }
            }
            break;
          }
          case OB_ALL_VIRTUAL_OUTLINE_TID: {
            ObVirtualOutline *outline = NULL;
            if (OB_SUCC(NEW_VIRTUAL_TABLE(ObVirtualOutline, outline))) {
              
              vt_iter = static_cast<ObVirtualTableIterator *>(outline);
            }
            break;
          }
          case OB_ALL_VIRTUAL_SYS_TASK_STATUS_TID: {
            ObAllVirtualSysTaskStatus *sys_task_status = NULL;
            if (OB_SUCC(NEW_VIRTUAL_TABLE(ObAllVirtualSysTaskStatus, sys_task_status))) {
              if (OB_FAIL(sys_task_status->init(SYS_TASK_STATUS_MGR))) {
              } else {
                vt_iter = static_cast<ObVirtualTableIterator *>(sys_task_status);
              }
            }
            break;
          }
          case OB_ALL_VIRTUAL_MACRO_BLOCK_MARKER_STATUS_TID: {
            ObAllVirtualMacroBlockMarkerStatus *all_virtual_marker_status = NULL;
            if (OB_SUCC(NEW_VIRTUAL_TABLE(ObAllVirtualMacroBlockMarkerStatus, all_virtual_marker_status))) {
              blocksstable::ObMacroBlockMarkerStatus marker_status;
              if (OB_FAIL(OB_SERVER_BLOCK_MGR.get_marker_status(marker_status))) {
              } else if (OB_FAIL(all_virtual_marker_status->init(marker_status))) {
              } else {
                vt_iter = static_cast<ObVirtualTableIterator *>(all_virtual_marker_status);
              }
            }
            break;
          }
          case OB_ALL_VIRTUAL_TABLET_BUFFER_INFO_TID: {
            ObAllVirtualTabletBufferInfo *all_virtual_tablet_buffer_info = NULL;
            if (OB_SUCC(NEW_VIRTUAL_TABLE(ObAllVirtualTabletBufferInfo, all_virtual_tablet_buffer_info))) {
              if (OB_FAIL(all_virtual_tablet_buffer_info->init(addr_))) {
              } else {
                vt_iter = static_cast<ObVirtualTableIterator *>(all_virtual_tablet_buffer_info);
              }
            }
            break;
          }
          case OB_ALL_VIRTUAL_LOCK_WAIT_STAT_TID: {
            ObAllVirtualLockWaitStat *lock_wait_stat = NULL;
            if (OB_SUCCESS == NEW_VIRTUAL_TABLE(ObAllVirtualLockWaitStat, lock_wait_stat)) {
              vt_iter = static_cast<ObVirtualTableIterator *>(lock_wait_stat);
            }
            break;
          }
        END_CREATE_VT_ITER_SWITCH_LAMBDA

        BEGIN_CREATE_VT_ITER_SWITCH_LAMBDA
          case OB_ALL_VIRTUAL_LONG_OPS_STATUS_TID: {
            ObAllVirtualLongOpsStatus *long_ops_status = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObAllVirtualLongOpsStatus, long_ops_status))) {
            } else {
              vt_iter = static_cast<ObVirtualTableIterator *>(long_ops_status);
            }
            break;
          }
          case OB_ALL_VIRTUAL_CHARSET_TID: {
            ObVirtualCharset *charset = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObVirtualCharset,
                                          charset))) {
            } else {
              vt_iter = static_cast<ObVirtualTableIterator *>(charset);
            }
            break;
          }
          case OB_ALL_VIRTUAL_COLLATION_TID: {
            ObVirtualCollation *collation = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObVirtualCollation,
                                          collation))) {
            } else {
              vt_iter = static_cast<ObVirtualTableIterator *>(collation);
            }
            break;
          }
          case OB_ALL_VIRTUAL_MEMSTORE_ALLOCATOR_INFO_TID: {
            ObAllVirtualMemstoreAllocatorInfo *memstore_allocator_info = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObAllVirtualMemstoreAllocatorInfo,
                                          memstore_allocator_info))) {
            } else {
              vt_iter = static_cast<ObVirtualTableIterator *>(memstore_allocator_info);
            }
            break;
          }
          case OB_ALL_VIRTUAL_IO_STAT_TID: {
            ObAllVirtualIOStat *all_io_stat = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObAllVirtualIOStat, all_io_stat))) {
            } else {
              vt_iter = static_cast<ObAllVirtualIOStat *>(all_io_stat);
            }
            break;
          }
          case OB_ALL_VIRTUAL_BAD_BLOCK_TABLE_TID: {
            ObVirtualBadBlockTable *bad_block_table = NULL;
            if (OB_SUCC(NEW_VIRTUAL_TABLE(ObVirtualBadBlockTable, bad_block_table))) {
              if (OB_FAIL(bad_block_table->init(addr_))) {
              } else {
                vt_iter = static_cast<ObVirtualTableIterator *>(bad_block_table);
              }
            }
            break;
          }
          case OB_ALL_VIRTUAL_DTL_CHANNEL_TID: {
            ObAllVirtualDtlChannel *dtl_ch = nullptr;
            if (OB_SUCC(NEW_VIRTUAL_TABLE(ObAllVirtualDtlChannel, dtl_ch))) {
              vt_iter = static_cast<ObVirtualTableIterator *>(dtl_ch);
            }
            break;
          }
          case OB_ALL_VIRTUAL_DTL_MEMORY_TID: {
            ObAllVirtualDtlMemory *dtl_mem = nullptr;
            if (OB_SUCC(NEW_VIRTUAL_TABLE(ObAllVirtualDtlMemory, dtl_mem))) {
              vt_iter = static_cast<ObVirtualTableIterator *>(dtl_mem);
            }
            break;
          }
          case OB_ALL_VIRTUAL_PX_WORKER_STAT_TID: {
            ObAllPxWorkerStatTable *px_worker_stat = NULL;
            if (OB_SUCC(NEW_VIRTUAL_TABLE(ObAllPxWorkerStatTable, px_worker_stat))) {
              px_worker_stat->set_allocator(&allocator);
              vt_iter = static_cast<ObVirtualTableIterator *>(px_worker_stat);
            }
            break;
          }
          case OB_ALL_VIRTUAL_PX_P2P_DATAHUB_TID: {
            ObAllPxP2PDatahubTable *px_p2p_datahub = NULL;
            if (OB_SUCC(NEW_VIRTUAL_TABLE(ObAllPxP2PDatahubTable, px_p2p_datahub))) {
              px_p2p_datahub->set_allocator(&allocator);
              vt_iter = static_cast<ObVirtualTableIterator *>(px_p2p_datahub);
            }
            break;
          }
          case OB_ALL_VIRTUAL_TABLET_STORE_STAT_TID: {
            ObAllVirtualTabletStoreStat *part_table_store_stat = NULL;
            if (OB_SUCC(NEW_VIRTUAL_TABLE(ObAllVirtualTabletStoreStat, part_table_store_stat))) {
              if (OB_FAIL(part_table_store_stat->init())) {
              } else {
                vt_iter = static_cast<ObVirtualTableIterator *>(part_table_store_stat);
              }
            }
            break;
          }
          case OB_ALL_VIRTUAL_SERVER_SCHEMA_INFO_TID: {
            ObAllVirtualServerSchemaInfo *server_schema_info = NULL;
            share::schema::ObMultiVersionSchemaService &schema_service =
                                                          local_management_service_.get_schema_service();
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObAllVirtualServerSchemaInfo,
                                          server_schema_info, schema_service))) {
            } else {
              vt_iter = static_cast<ObVirtualTableIterator *>(server_schema_info);
            }
            break;
          }
          case OB_ALL_VIRTUAL_SCHEMA_MEMORY_TID: {
            ObAllVirtualSchemaMemory *schema_memory = NULL;
            share::schema::ObMultiVersionSchemaService &schema_service =
                                                          local_management_service_.get_schema_service();
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObAllVirtualSchemaMemory,
                                          schema_memory, schema_service))) {
            } else {
              vt_iter = static_cast<ObVirtualTableIterator *>(schema_memory);
            }
            break;
          }
          case OB_ALL_VIRTUAL_SCHEMA_SLOT_TID: {
            ObAllVirtualSchemaSlot *schema_slot = NULL;
            share::schema::ObMultiVersionSchemaService &schema_service =
                                                          local_management_service_.get_schema_service();
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObAllVirtualSchemaSlot,
                                          schema_slot, schema_service))) {
            } else {
              vt_iter = static_cast<ObVirtualTableIterator *>(schema_slot);
            }
            break;
          }
        END_CREATE_VT_ITER_SWITCH_LAMBDA

        BEGIN_CREATE_VT_ITER_SWITCH_LAMBDA
          case OB_ALL_VIRTUAL_MEMORY_CONTEXT_STAT_TID: {
            ObAllVirtualMemoryContextStat *memory_context_stat = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObAllVirtualMemoryContextStat, memory_context_stat))) {
            } else {
              vt_iter = static_cast<ObVirtualTableIterator *>(memory_context_stat);
            }
            break;
          }
          case OB_ALL_VIRTUAL_DUMP_INFO_TID: {
            ObAllVirtualDumpInfo *dump_info = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObAllVirtualDumpInfo, dump_info))) {
            } else {
              vt_iter = static_cast<ObVirtualTableIterator *>(dump_info);
            }
            break;
          }
          case OB_ALL_VIRTUAL_SQL_WORKAREA_HISTORY_STAT_TID: {
            ObSqlWorkareaHistoryStat *wa_stat = NULL;
            if (OB_SUCC(NEW_VIRTUAL_TABLE(ObSqlWorkareaHistoryStat, wa_stat))) {
              vt_iter = static_cast<ObSqlWorkareaHistoryStat *>(wa_stat);
            }
            break;
          }
          case OB_ALL_VIRTUAL_SQL_WORKAREA_ACTIVE_TID: {
            ObSqlWorkareaActive *wa_active = NULL;
            if (OB_SUCC(NEW_VIRTUAL_TABLE(ObSqlWorkareaActive, wa_active))) {
              vt_iter = static_cast<ObSqlWorkareaActive *>(wa_active);
            }
            break;
          }
          case OB_ALL_VIRTUAL_SQL_WORKAREA_HISTOGRAM_TID: {
            ObSqlWorkareaHistogram *wa_hist = NULL;
            if (OB_SUCC(NEW_VIRTUAL_TABLE(ObSqlWorkareaHistogram, wa_hist))) {
              vt_iter = static_cast<ObSqlWorkareaHistogram *>(wa_hist);
            }
            break;
          }
          case OB_ALL_VIRTUAL_SQL_WORKAREA_MEMORY_INFO_TID: {
            ObSqlWorkareaMemoryInfo *wa_memory_info = NULL;
            if (OB_SUCC(NEW_VIRTUAL_TABLE(ObSqlWorkareaMemoryInfo, wa_memory_info))) {
              vt_iter = static_cast<ObSqlWorkareaMemoryInfo *>(wa_memory_info);
            }
            break;
          }
          case OB_ALL_VIRTUAL_SERVER_STAT_TID: {
            ObAllVirtualServer *server = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObAllVirtualServer, server))) {
            } else if (OB_FAIL(server->init(addr_))) {
            } else {
              vt_iter = static_cast<ObVirtualTableIterator *>(server);
            }
            break;
          }
          case OB_ALL_VIRTUAL_ID_SERVICE_TID: {
            ObAllVirtualIDService *id_service = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObAllVirtualIDService, id_service))) {
            } else {
              vt_iter = static_cast<ObVirtualTableIterator *>(id_service);
            }
            break;
          }
          case OB_ALL_VIRTUAL_TIMESTAMP_SERVICE_TID: {
            ObAllVirtualTimestampService *timestamp_service = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObAllVirtualTimestampService, timestamp_service))) {
            } else {
              vt_iter = static_cast<ObVirtualTableIterator *>(timestamp_service);
            }
            break;
          }
          case OB_ALL_VIRTUAL_OPEN_CURSOR_TID: {
            ObVirtualOpenCursorTable *open_cursors = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObVirtualOpenCursorTable, open_cursors))) {
            } else {
              open_cursors->set_allocator(&allocator);
              open_cursors->sesession_pool(::oceanbase::share::server_service<::oceanbase::sql::ObSQLSessionMgr>());
              OZ (open_cursors->set_addr(addr_));
              OX (vt_iter = static_cast<ObVirtualOpenCursorTable*>(open_cursors));
            }
            break;
          }
          case OB_ALL_VIRTUAL_DAG_WARNING_HISTORY_TID: {
            ObAllVirtualDagWarningHistory *dag_warning_mgr = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObAllVirtualDagWarningHistory, dag_warning_mgr))) {
            } else {
              vt_iter = static_cast<ObVirtualTableIterator *>(dag_warning_mgr);
            }
            break;
          }
          case OB_ALL_VIRTUAL_DAG_TID: {
            ObAllVirtualDag *dag_info_mgr = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObAllVirtualDag, dag_info_mgr))) {
            } else if (OB_FAIL(dag_info_mgr->init())) {
            } else {
              vt_iter = static_cast<ObVirtualTableIterator *>(dag_info_mgr);
            }
            break;
          }
          case OB_ALL_VIRTUAL_DAG_SCHEDULER_TID: {
            ObAllVirtualDagScheduler *dag_scheduler_info_mgr = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObAllVirtualDagScheduler, dag_scheduler_info_mgr))) {
            } else if (OB_FAIL(dag_scheduler_info_mgr->init())) {
            } else {
              vt_iter = static_cast<ObVirtualTableIterator *>(dag_scheduler_info_mgr);
            }
            break;
          }
          case OB_ALL_VIRTUAL_SERVER_COMPACTION_PROGRESS_TID: {
            ObAllVirtualServerCompactionProgress *progress_mgr = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObAllVirtualServerCompactionProgress, progress_mgr))) {
            } else if (OB_FAIL(progress_mgr->init())) {
            } else {
              vt_iter = static_cast<ObVirtualTableIterator *>(progress_mgr);
            }
            break;
          }
          case OB_ALL_VIRTUAL_SERVER_COMPACTION_EVENT_HISTORY_TID: {
            ObAllVirtualServerCompactionEventHistory *event_history = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObAllVirtualServerCompactionEventHistory, event_history))) {
            } else if (OB_FAIL(event_history->init())) {
            } else {
              vt_iter = static_cast<ObVirtualTableIterator *>(event_history);
            }
            break;
          }
          case OB_ALL_VIRTUAL_TABLET_STAT_TID: {
            ObAllVirtualTabletStat *tablet_stat = nullptr;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObAllVirtualTabletStat, tablet_stat))) {
            } else {
              vt_iter = static_cast<ObVirtualTableIterator *>(tablet_stat);
            }
            break;
          }
          case OB_ALL_VIRTUAL_TABLET_COMPACTION_PROGRESS_TID:{
            ObAllVirtualTabletCompactionProgress *progress_mgr = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObAllVirtualTabletCompactionProgress, progress_mgr))) {
            } else if (OB_FAIL(progress_mgr->init())) {
            } else {
              vt_iter = static_cast<ObVirtualTableIterator *>(progress_mgr);
            }
            break;
          }

          case OB_ALL_VIRTUAL_COMPACTION_DIAGNOSE_INFO_TID:{
            ObAllVirtualCompactionDiagnoseInfo *info_mgr = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObAllVirtualCompactionDiagnoseInfo, info_mgr))) {
            } else if (OB_FAIL(info_mgr->init())) {
            } else {
              vt_iter = static_cast<ObVirtualTableIterator *>(info_mgr);
            }
            break;
          }

          case OB_ALL_VIRTUAL_COMPACTION_SUGGESTION_TID: {
            ObAllVirtualCompactionSuggestion *suggestion_mgr = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObAllVirtualCompactionSuggestion, suggestion_mgr))) {
            } else if (OB_FAIL(suggestion_mgr->init())) {
            } else {
              vt_iter = static_cast<ObVirtualTableIterator *>(suggestion_mgr);
            }
            break;
          }
          case OB_ALL_VIRTUAL_TABLET_COMPACTION_HISTORY_TID: {
            ObAllVirtualTabletCompactionHistory *history = NULL;
            if (OB_SUCC(NEW_VIRTUAL_TABLE(ObAllVirtualTabletCompactionHistory, history))) {
              vt_iter = static_cast<ObVirtualTableIterator *>(history);
            }
            break;
          }
          case OB_ALL_VIRTUAL_IO_CALIBRATION_STATUS_TID: {
            ObAllVirtualIOCalibrationStatus *calibration_status = nullptr;
            if (OB_SUCC(NEW_VIRTUAL_TABLE(ObAllVirtualIOCalibrationStatus, calibration_status))) {
              if (OB_FAIL(calibration_status->init(addr_))) {
              } else {
                vt_iter = static_cast<ObVirtualTableIterator *>(calibration_status);
              }
            }
            break;
          }
          case OB_ALL_VIRTUAL_IO_BENCHMARK_TID: {
            ObAllVirtualIOBenchmark *io_benchmark = nullptr;
            if (OB_SUCC(NEW_VIRTUAL_TABLE(ObAllVirtualIOBenchmark, io_benchmark))) {
              if (OB_FAIL(io_benchmark->init(addr_))) {
              } else {
                vt_iter = static_cast<ObVirtualTableIterator *>(io_benchmark);
              }
            }
            break;
          }
          case OB_ALL_VIRTUAL_IO_QUOTA_TID: {
            ObAllVirtualIOQuota *io_quota= nullptr;
            if (OB_SUCC(NEW_VIRTUAL_TABLE(ObAllVirtualIOQuota, io_quota))) {
              if (OB_FAIL(io_quota->init(addr_))) {
              } else {
                vt_iter = static_cast<ObVirtualTableIterator *>(io_quota);
              }
            }
            break;
          }
          case OB_ALL_VIRTUAL_GROUP_IO_STAT_TID: {
            ObAllVirtualGroupIOStat *group_io_stat= nullptr;
            if (OB_SUCC(NEW_VIRTUAL_TABLE(ObAllVirtualGroupIOStat, group_io_stat))) {
              if (OB_FAIL(group_io_stat->init(addr_))) {
              } else {
                vt_iter = static_cast<ObVirtualTableIterator *>(group_io_stat);
              }
            }
            break;
          }
          case OB_ALL_VIRTUAL_DDL_SIM_POINT_TID: {
            ObAllVirtualDDLSimPoint *ddl_sim_point = nullptr;
            if (OB_SUCC(NEW_VIRTUAL_TABLE(ObAllVirtualDDLSimPoint, ddl_sim_point))) {
              vt_iter = static_cast<ObVirtualTableIterator *>(ddl_sim_point);
            }
            break;
          }
          case OB_ALL_VIRTUAL_DDL_SIM_POINT_STAT_TID: {
            ObAllVirtualDDLSimPointStat *ddl_sim_point_stat = nullptr;
            if (OB_SUCC(NEW_VIRTUAL_TABLE(ObAllVirtualDDLSimPointStat, ddl_sim_point_stat))) {
              if (OB_FAIL(ddl_sim_point_stat->init(addr_))) {
              } else {
                vt_iter = static_cast<ObVirtualTableIterator *>(ddl_sim_point_stat);
              }
            }
            break;
          }
          case OB_ALL_VIRTUAL_TABLET_COMPACTION_INFO_TID: {
            ObAllVirtualTabletCompactionInfo *info_mgr = NULL;
            if (OB_SUCC(NEW_VIRTUAL_TABLE(ObAllVirtualTabletCompactionInfo, info_mgr))) {
              if (OB_FAIL(info_mgr->init(&allocator, addr_))) {
              } else {
                vt_iter = static_cast<ObVirtualTableIterator *>(info_mgr);
              }
            }
            break;
          }
        END_CREATE_VT_ITER_SWITCH_LAMBDA

        BEGIN_CREATE_VT_ITER_SWITCH_LAMBDA
          case OB_ALL_VIRTUAL_CTX_MEMORY_INFO_TID: {
            ObAllVirtualCtxMemoryInfo *ctx_memory_info = NULL;
            if (OB_SUCC(NEW_VIRTUAL_TABLE(ObAllVirtualCtxMemoryInfo,
                                          ctx_memory_info))) {
              ctx_memory_info->set_allocator(&allocator);
              vt_iter = static_cast<ObVirtualTableIterator *>(ctx_memory_info);
            }
            break;
          }
          case OB_ALL_VIRTUAL_PX_TARGET_MONITOR_TID: {
            ObAllVirtualPxTargetMonitor *all_px_target_monitor = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObAllVirtualPxTargetMonitor, all_px_target_monitor))) {
            } else if (OB_FAIL(all_px_target_monitor->init())) {
            } else {
              vt_iter = static_cast<ObVirtualTableIterator *>(all_px_target_monitor);
            }
            break;
          }
          case OB_ALL_VIRTUAL_LOAD_DATA_STAT_TID: {
            ObAllVirtualLoadDataStat *load_data_stat = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObAllVirtualLoadDataStat, load_data_stat))) {
            } else {
              vt_iter = static_cast<ObVirtualTableIterator *>(load_data_stat);
            }
            break;
          }
          case OB_ALL_VIRTUAL_KVCACHE_STORE_MEMBLOCK_TID: {
            ObAllVirtualKVCacheStoreMemblock *kvcache_store_memblock = nullptr;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObAllVirtualKVCacheStoreMemblock, kvcache_store_memblock))) {
            } else {
              kvcache_store_memblock->set_addr(addr_);
              vt_iter = static_cast<ObVirtualTableIterator *>(kvcache_store_memblock);
            }
            break;
          }
          case OB_ALL_VIRTUAL_TRACEPOINT_INFO_TID: {
            ObAllTracepointInfo *tp_info = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObAllTracepointInfo, tp_info))) {
            } else {
              vt_iter = static_cast<ObVirtualTableIterator *>(tp_info);
            }
            break;
          }
          case OB_ALL_VIRTUAL_DTL_INTERM_RESULT_MONITOR_TID: {
            ObAllDtlIntermResultMonitor *dtl_interm_result_monitor = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObAllDtlIntermResultMonitor, dtl_interm_result_monitor))) {
            } else {
              vt_iter = static_cast<ObVirtualTableIterator *>(dtl_interm_result_monitor);
            }
            break;
          }
          case OB_ALL_VIRTUAL_DML_STATS_TID: {
            ObAllVirtualDMmlStats *dml_stats_result_monitor = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObAllVirtualDMmlStats, dml_stats_result_monitor))) {
            } else {
              vt_iter = static_cast<ObVirtualTableIterator *>(dml_stats_result_monitor);
            }
            break;
          }
          case OB_ALL_VIRTUAL_MDS_NODE_STAT_TID: {
            ObAllVirtualMdsNodeStat *mds_node_stat = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObAllVirtualMdsNodeStat, mds_node_stat))) {
            } else {
              vt_iter = static_cast<ObAllVirtualMdsNodeStat *>(mds_node_stat);
            }
            break;
          }
          case OB_ALL_VIRTUAL_MDS_EVENT_HISTORY_TID: {
            ObAllVirtualMdsEventHistory *mds_event_history = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObAllVirtualMdsEventHistory, mds_event_history))) {
            } else {
              vt_iter = static_cast<ObAllVirtualMdsEventHistory *>(mds_event_history);
            }
            break;
          }
          case OB_ALL_VIRTUAL_SQL_PLAN_TID: {
            ObAllVirtualSqlPlan *sql_plan_table = NULL;
            if (OB_SUCC(NEW_VIRTUAL_TABLE(ObAllVirtualSqlPlan, sql_plan_table))) {
              sql_plan_table->set_allocator(&allocator);
              vt_iter = static_cast<ObVirtualTableIterator *>(sql_plan_table);
            }
            break;
          }
          case OB_ALL_VIRTUAL_OPT_STAT_GATHER_MONITOR_TID: {
            ObAllVirtualOptStatGatherMonitor *opt_stats_gather_stat = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObAllVirtualOptStatGatherMonitor, opt_stats_gather_stat))) {
            } else {
              opt_stats_gather_stat->set_allocator(&allocator);
              opt_stats_gather_stat->set_addr(addr_);
              vt_iter = static_cast<ObVirtualTableIterator *>(opt_stats_gather_stat);
            }
            break;
          }
          case OB_ALL_VIRTUAL_RESOURCE_LIMIT_TID: {
            ObResourceLimitTable *all_virtual_resource_limit = NULL;
            if (OB_SUCC(NEW_VIRTUAL_TABLE(ObResourceLimitTable,
                                          all_virtual_resource_limit))) {
              vt_iter = static_cast<ObResourceLimitTable *>(all_virtual_resource_limit);
            }
            break;
          }
          case OB_ALL_VIRTUAL_RESOURCE_LIMIT_DETAIL_TID: {
            ObResourceLimitDetailTable *all_virtual_resource_limit_detail = NULL;
            if (OB_SUCC(NEW_VIRTUAL_TABLE(ObResourceLimitDetailTable,
                                          all_virtual_resource_limit_detail))) {
              vt_iter = static_cast<ObResourceLimitDetailTable *>(all_virtual_resource_limit_detail);
            }
            break;
          }
          case OB_ALL_VIRTUAL_DATA_ACTIVITY_METRICS_TID: {
            ObAllVirtualActivityMetric *all_virtual_activity_metrics = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObAllVirtualActivityMetric, all_virtual_activity_metrics))) {
            } else {
              vt_iter = static_cast<ObVirtualTableIterator *>(all_virtual_activity_metrics);
            }
            break;
          }
          case OB_ALL_VIRTUAL_NIC_INFO_TID: {
            ObAllVirtualNicInfo *nic_info = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObAllVirtualNicInfo, nic_info))) {
            } else {
              vt_iter = static_cast<ObVirtualTableIterator *>(nic_info);
            }
            break;
          }
          case OB_ALL_VIRTUAL_SYS_VARIABLE_DEFAULT_VALUE_TID: {
            ObSysVarDefaultValue *sys_var_default_value = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObSysVarDefaultValue, sys_var_default_value))) {
            } else {
              vt_iter = static_cast<ObVirtualTableIterator *>(sys_var_default_value);
            }
            break;
          }
          case OB_ENABLED_ROLES_TID:
          {
            ObInfoSchemaEnableRolesTable *enable_roles = NULL;
            if (OB_SUCC(NEW_VIRTUAL_TABLE(ObInfoSchemaEnableRolesTable, enable_roles))) {
              vt_iter = static_cast<ObVirtualTableIterator *>(enable_roles);
            }
            break;
          }
          case OB_ALL_VIRTUAL_SCHEDULER_RUNNING_JOB_TID:
          {
            ObAllVirtualSchedulerRunningJob *running_job = NULL;
            if (OB_SUCC(NEW_VIRTUAL_TABLE(ObAllVirtualSchedulerRunningJob, running_job))) {
              running_job->sesession_pool(::oceanbase::share::server_service<::oceanbase::sql::ObSQLSessionMgr>());
              vt_iter = static_cast<ObVirtualTableIterator *>(running_job);
            }
            break;
          }
          case OB_ALL_VIRTUAL_SQLSTAT_TID: {
            ObAllVirtualSqlStat *all_virtual_sqlstat = NULL;
            if (OB_SUCC(NEW_VIRTUAL_TABLE(ObAllVirtualSqlStat, all_virtual_sqlstat))) {
              all_virtual_sqlstat->set_allocator(&allocator);
              vt_iter = static_cast<ObVirtualTableIterator *>(all_virtual_sqlstat);
            }
            break;
          }
          case OB_ALL_VIRTUAL_TEMP_FILE_TID:
          {
            ObAllVirtualTmpFileInfo *all_tmp_file_info = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObAllVirtualTmpFileInfo, all_tmp_file_info))) {
            } else {
              vt_iter = static_cast<ObVirtualTableIterator *>(all_tmp_file_info);
            }
            break;
          }
          case OB_ALL_VIRTUAL_VECTOR_INDEX_INFO_TID:
          {
            ObAllVirtualVectorIndexInfo *all_virtual_vector_index_info = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObAllVirtualVectorIndexInfo, all_virtual_vector_index_info))) {
            } else {
              all_virtual_vector_index_info->set_addr(addr_);
              vt_iter = static_cast<ObVirtualTableIterator *>(all_virtual_vector_index_info);
            }
            break;
          }
          case OB_ALL_VIRTUAL_CHANGE_STREAM_REFRESH_STAT_TID:
          {
            ObAllVirtualChangeStreamRefreshStat *change_stream_refresh_stat = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObAllVirtualChangeStreamRefreshStat, change_stream_refresh_stat))) {
            } else {
              vt_iter = static_cast<ObVirtualTableIterator *>(change_stream_refresh_stat);
            }
            break;
          }
          case OB_ALL_VIRTUAL_VECTOR_MEM_INFO_TID:
          {
            ObAllVirtualVectorMemInfo *vector_mem_info = NULL;
            if (OB_FAIL(NEW_VIRTUAL_TABLE(ObAllVirtualVectorMemInfo, vector_mem_info))) {
            } else {
              vt_iter = static_cast<ObVirtualTableIterator *>(vector_mem_info);
            }
            break;
          }
        END_CREATE_VT_ITER_SWITCH_LAMBDA

#define LOCAL_VIRTUAL_TABLE_CREATE_ITER
#include "share/inner_table/ob_inner_table_schema_misc.ipp"
#undef LOCAL_VIRTUAL_TABLE_CREATE_ITER

    if (OB_SUCC(ret) && !processed) {
      ret = OB_INVALID_ARGUMENT;
      SERVER_LOG(WARN, "unknown system table function provider", K(ret), K(pure_tid));
    }
    if (OB_SUCC(ret) && OB_ISNULL(vt_iter)) {
      ret = OB_ERR_UNEXPECTED;
    }
  }
  return ret;
}

int ObVTIterCreator::check_can_create_iter(ObVTableScanParam &params)
{
  return TableFunctionRegistry::find(params.index_id_) != nullptr ? OB_SUCCESS : OB_INVALID_ARGUMENT;
}



// Adapts the existing collectors to an execution-local table-function scan.
// Each rescan owns a new collector and arena so cursors, guards and sampled rows
// cannot escape to another invocation or accumulate with correlated outer rows.
class SystemTableFunctionScan final : public sql::ITableFunctionScan
{
public:
  explicit SystemTableFunctionScan(const sql::TableFunctionScanContext &context)
    : exec_ctx_(context.exec_ctx_), definition_(context.descriptor_),
      lookups_(context.lookups_), allocator_("SysTableFunc"), schema_guard_(ObSchemaMgrItem::MOD_VTABLE_SCAN_PARAM),
      address_(ObServer::get_instance().get_self()),
      creator_(ObServer::get_instance().get_local_management_service(), address_),
      iterator_(nullptr)
  {}
  ~SystemTableFunctionScan() override { close(); }

  int initialize(const sql::TableFunctionScanContext &context)
  {
    int ret = column_ids_.assign(context.column_ids_);
    if (OB_SUCC(ret)) { ret = prepare_arguments(context.arguments_); }
    return ret;
  }

  int open() override
  {
    int ret = OB_SUCCESS;
    if (nullptr != iterator_) {
      ret = OB_INIT_TWICE;
    } else if (OB_ISNULL(exec_ctx_.get_my_session()) || OB_ISNULL(GCTX.schema_service_)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(schema_guard_))) {
    } else {
      ObQueryFlag scan_flag;
      if (OB_FAIL(creator_.create_function_iter(definition_, allocator_,
                    exec_ctx_.get_my_session(), schema_guard_, ranges_, scan_flag, iterator_))) {
      } else if (OB_FAIL(iterator_->set_output_column_ids(column_ids_))) {
      } else {
        iterator_->set_allocator(&allocator_);
        iterator_->set_reserved_column_cnt(column_ids_.count());
        const ObPhysicalPlanCtx *plan_ctx = exec_ctx_.get_physical_plan_ctx();
        iterator_->set_timeout(plan_ctx == nullptr ? INT64_MAX : plan_ctx->get_timeout_timestamp());
        ret = iterator_->open();
      }
    }
    if (OB_FAIL(ret)) {
      const int saved_ret = ret;
      close();
      ret = saved_ret;
    }
    return ret;
  }

  int get_next_row(ObNewRow *&row) override
  {
    return iterator_ == nullptr ? OB_NOT_INIT : iterator_->get_next_row(row);
  }

  int rescan(const ObIArray<ObObj> &arguments) override
  {
    int ret = close();
    if (OB_SUCC(ret)) { ret = prepare_arguments(arguments); }
    if (OB_SUCC(ret)) { ret = open(); }
    return ret;
  }

  int close() override
  {
    int ret = OB_SUCCESS;
    if (iterator_ != nullptr) {
      ret = iterator_->close();
      iterator_->~ObVirtualTableIterator();
      iterator_ = nullptr;
    }
    ranges_.reuse();
    schema_guard_.reset();
    allocator_.reset();
    return ret;
  }

private:
  int prepare_arguments(const ObIArray<ObObj> &arguments)
  {
    int ret = OB_SUCCESS;
    if (arguments.count() < definition_.min_arguments_
        || (definition_.max_arguments_ >= 0 && arguments.count() > definition_.max_arguments_)) {
      ret = OB_INVALID_ARGUMENT;
    } else if (definition_.id_ == OB_ALL_VIRTUAL_INFORMATION_COLUMNS_TID) {
      ObSEArray<ObObj, 2> located_arguments;
      // A database is needed before a table name can avoid global enumeration.
      // Explicit arguments take priority; residual SQL filters still apply.
      for (int64_t i = 0; OB_SUCC(ret) && i < definition_.lookup_column_count_; ++i) {
        const ObObj *value = i < arguments.count() ? &arguments.at(i) : nullptr;
        for (int64_t j = 0; value == nullptr && j < lookups_.count(); ++j) {
          if (lookups_.at(j).column_id_ == definition_.lookup_column_ids_[i]) {
            value = &lookups_.at(j).value_;
          }
        }
        if (value == nullptr) { break; }
        ret = located_arguments.push_back(*value);
      }
      if (OB_SUCC(ret)) { ret = build_key_ranges(located_arguments); }
    } else {
      ret = build_key_ranges(arguments);
    }
    return ret;
  }

  int build_key_ranges(const ObIArray<ObObj> &arguments)
  {
    int ret = OB_SUCCESS;
    if (arguments.count() > 0) {
      const bool names = definition_.id_ == OB_ALL_VIRTUAL_INFORMATION_COLUMNS_TID;
      const bool tablet_ids = definition_.id_ == OB_ALL_VIRTUAL_MDS_NODE_STAT_TID;
      const bool separate_ranges = tablet_ids || definition_.id_ == OB_ALL_VIRTUAL_PRIVILEGE_GRANT_TID;
      const ObObjType key_type = names ? ObVarcharType : tablet_ids ? ObUInt64Type : ObIntType;
      const int64_t key_count = names ? 2 : arguments.count();
      void *storage = allocator_.alloc(sizeof(ObObj) * key_count);
      if (OB_ISNULL(storage) || OB_ISNULL(exec_ctx_.get_my_session())) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        ObObj *keys = new (storage) ObObj[key_count];
        const ObDataTypeCastParams dtc_params =
            ObBasicSessionInfo::create_dtc_params(exec_ctx_.get_my_session());
        ObCastCtx cast_ctx(&allocator_, &dtc_params, CM_NONE,
                          ObCharset::get_default_collation(ObCharset::get_default_charset()));
        for (int64_t i = 0; OB_SUCC(ret) && i < arguments.count(); ++i) {
          ObObj converted;
          if (arguments.at(i).is_null()) {
            ret = OB_INVALID_ARGUMENT;
          } else if (OB_FAIL(ObObjCaster::to_type(key_type,
                             names ? ObCharset::get_default_collation(ObCharset::get_default_charset())
                                   : CS_TYPE_BINARY,
                             cast_ctx, arguments.at(i), converted))) {
          } else {
            ret = ob_write_obj(allocator_, converted, keys[i]);
          }
        }
        if (OB_SUCC(ret)) {
          if (names && arguments.count() == 1) { keys[1].set_max_value(); }
          for (int64_t i = 0; OB_SUCC(ret) && i < (separate_ranges ? arguments.count() : 1); ++i) {
            ObNewRange range;
            range.start_key_.assign(keys + (separate_ranges ? i : 0), separate_ranges ? 1 : key_count);
            range.end_key_ = range.start_key_;
            range.border_flag_.set_inclusive_start();
            range.border_flag_.set_inclusive_end();
            ret = ranges_.push_back(range);
          }
        }
      }
    }
    return ret;
  }

  ObExecContext &exec_ctx_;
  const TableFunctionDescriptor &definition_;
  const ObIArray<TableFunctionLookup> &lookups_;
  ObArenaAllocator allocator_;
  ObSchemaGetterGuard schema_guard_;
  ObAddr address_;
  ObVTIterCreator creator_;
  ObSEArray<uint64_t, 64> column_ids_;
  ObSEArray<ObNewRange, 4> ranges_;
  ObVirtualTableIterator *iterator_;
};

int create_system_table_function_scan(const TableFunctionScanContext &context,
                                      ITableFunctionScan *&scan)
{
  int ret = OB_SUCCESS;
  scan = nullptr;
  void *storage = context.allocator_.alloc(sizeof(SystemTableFunctionScan));
  if (OB_ISNULL(storage)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    auto *provider = new (storage) SystemTableFunctionScan(context);
    if (OB_FAIL(provider->initialize(context))) {
      provider->~SystemTableFunctionScan();
      context.allocator_.free(storage);
    } else {
      scan = provider;
    }
  }
  return ret;
}

}/* ns observer*/
}/* ns oceanbase */

namespace oceanbase {
namespace sql {
using namespace common;
using namespace share;

static TableFunctionColumn make_system_table_column(const char *name, ObObjType type,
    ObCollationType collation, int64_t length, int16_t precision, int16_t scale, bool nullable, uint64_t id)
{
  TableFunctionColumn column;
  column.name_ = name;
  column.id_ = id;
  column.nullable_ = nullable;
  column.type_.set_obj_type(type);
  // Match the SQL type normalization used by inner-table column definitions,
  // without creating column or table schema objects.
  const ObAccuracy &defaults = ObAccuracy::DDL_DEFAULT_ACCURACY[type];
  if (ob_is_string_tc(type)) {
    column.type_.set_length(static_cast<ObLength>(length));
    column.type_.set_precision(precision);
  } else if (ob_is_text_tc(type) || ob_is_json_tc(type) || ob_is_geometry_tc(type)) {
    column.type_.set_length(defaults.get_length());
  } else if (ob_is_datetime_tc(type) || ob_is_time_tc(type)) {
    const ObScale resolved_scale = scale == -1 ? defaults.get_scale() : scale;
    column.type_.set_precision(static_cast<ObPrecision>(defaults.get_precision() + resolved_scale));
    column.type_.set_scale(resolved_scale);
  } else {
    column.type_.set_precision(precision == -1 ? defaults.get_precision() : precision);
    column.type_.set_scale(scale == -1 ? defaults.get_scale() : scale);
  }
  if (CS_TYPE_INVALID == collation) {
    collation = ob_is_string_type(type)
        ? ObCharset::get_default_collation(ObCharset::get_default_charset()) : CS_TYPE_BINARY;
  }
  column.type_.set_collation_type(collation);
  column.type_.set_charset_type(ObCharset::charset_type_by_coll(collation));
  column.type_.set_collation_level(ObRawExprUtils::get_column_collation_level(type));
  return column;
}

const TableFunctionDescriptor *get_system_table_functions(int64_t &count)
{
#define SYSTEM_TABLE_FUNCTION_DEFINITIONS
#include "share/inner_table/ob_inner_table_schema_misc.ipp"
#undef SYSTEM_TABLE_FUNCTION_DEFINITIONS
  count = ARRAYSIZEOF(system_table_functions);
  return system_table_functions;
}
} // namespace sql
} // namespace oceanbase

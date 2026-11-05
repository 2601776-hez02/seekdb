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

#ifndef OCEANBASE_OBSERVER_VIRTUAL_TABLE_OB_ALL_VIRTUAL_TABLE_ITERATOR_FACTORY_
#define OCEANBASE_OBSERVER_VIRTUAL_TABLE_OB_ALL_VIRTUAL_TABLE_ITERATOR_FACTORY_

#include "share/ob_define.h"
#include "lib/container/ob_se_array.h"
#include "observer/virtual_table/ob_virtual_table_iterator.h"
#include "share/config/ob_server_config.h"
#include "sql/engine/table/ob_i_virtual_table_iterator_factory.h"

namespace oceanbase
{
namespace sql
{
// Observer supplies the program-owned system catalog to the SQL registry at startup.
const TableFunctionDescriptor *get_system_table_functions(int64_t &count);
}
namespace common
{
class ObVTableScanParam;
class ObVirtualTableIterator;
}
namespace share
{
namespace schema
{
class ObSchemaGetterGuard;
class ObTableSchema;
}
}
namespace rootserver
{
class ObLocalManagementService;
}
namespace observer
{
class ObVTIterCreator
{
public:
  ObVTIterCreator(rootserver::ObLocalManagementService &local_management_service, common::ObAddr &addr)
    : local_management_service_(local_management_service),
      addr_(addr)
  {}
  virtual ~ObVTIterCreator() {}
  virtual int create_vt_iter(common::ObVTableScanParam &params,
                             common::ObVirtualTableIterator *&vt_iter);
  virtual int check_can_create_iter(common::ObVTableScanParam &params);
  int create_function_iter(const sql::TableFunctionDescriptor &definition,
                           common::ObIAllocator &allocator,
                           sql::ObSQLSessionInfo *session,
                           share::schema::ObSchemaGetterGuard &schema_guard,
                           const common::ObIArray<common::ObNewRange> &key_ranges,
                           const common::ObQueryFlag &scan_flag,
                           common::ObVirtualTableIterator *&vt_iter);
  rootserver::ObLocalManagementService &get_local_management_service() { return local_management_service_; }

private:
  rootserver::ObLocalManagementService &local_management_service_;
  common::ObAddr &addr_;
};

class ObVirtualTableIteratorFactory : public sql::ObIVirtualTableIteratorFactory
{
public:
  explicit ObVirtualTableIteratorFactory(ObVTIterCreator &vt_iter_creator);
  ObVirtualTableIteratorFactory(rootserver::ObLocalManagementService &local_management_service,
                                common::ObAddr &addr);
  virtual ~ObVirtualTableIteratorFactory();
  virtual int create_virtual_table_iterator(common::ObVTableScanParam &params,
                                            common::ObVirtualTableIterator *&vt_iter);
  virtual int revert_virtual_table_iterator(common::ObVirtualTableIterator *vt_iter);
  virtual int check_can_create_iter(common::ObVTableScanParam &params);
  ObVTIterCreator &get_vt_iter_creator() { return vt_iter_creator_; }
private:
  ObVTIterCreator vt_iter_creator_;
  DISALLOW_COPY_AND_ASSIGN(ObVirtualTableIteratorFactory);
};

}
}
#endif /* OCEANBASE_OBSERVER_VIRTUAL_TABLE_OB_ALL_VIRTUAL_TABLE_ITERATOR_FACTORY_ */

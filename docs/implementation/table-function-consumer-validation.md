# 表函数消费者切换验收

2026-10-17，在独立新实例 `tf-cutover-1`（MySQL 端口 22885）上执行；基线来自最新 master 的独立实例（22881）。快照对照期间在两实例建立临时测试用户和库，完成后均已清理。生产构建、provider 全量扫描、schema/磁盘统计由集成验证另行记录。

| 范围 | 结果 |
| --- | --- |
| 全部 139 个系统视图 `SELECT * ... LIMIT 1` | 138 成功，输出列名及顺序与基线一致；唯一错误 `V$OB_PX_TARGET_MONITOR` 的 4016 也存在于基线，没有新增失败 |
| SHOW、真实 DDL、权限与角色矩阵 | 93 项检查中 92 项通过；另一项是已确认的既有 PROCESSLIST 行可见性缺陷，见下文 |
| information_columns 定位和 SQL 过滤 | 29 项通过，包含大小写、尾空格、二进制比较、NULL 等值、隐式数值转换、矛盾条件、LEFT JOIN、关联实参、预处理参数变化及 NULL |
| 既有 mysqltest `view/check_privilege` | 扩展用例后通过实际 record 和独立比较执行；预期文件来自运行结果 |
| 全部 139 个系统视图 `DESC` | 列名、类型、可空性、Key 和 Extra 与 master 逐列一致；64 列的 DEFAULT 按函数输出无存储列默认值的规则由空白变为 NULL |
| 既有 INFORMATION_SCHEMA 两套快照 | `information_schema.test` 与 `information_schema_desc.test` 实际 record 后独立比较均为 `ok` |
| 临时数据清理 | 测试库、用户和角色均已删除，随后查询真实系统元数据确认没有遗留 |

SHOW 矩阵包括普通/完整表列表及 LIKE、列信息、索引、数据库/表/视图/存储函数/存储过程/触发器的 SHOW CREATE、角色参与的 SHOW GRANTS、变量、状态、参数、字符集、排序规则、警告与错误。真实 DDL 包括建立数据库、表、索引、视图和存储程序，插入数据，增加/删除列及重命名表。

权限检查使用仅获得单表 SELECT 的普通用户：SHOW 和 INFORMATION_SCHEMA 只列出获授表；未授权表的 SELECT、SHOW COLUMNS、SHOW CREATE 被拒绝；直接访问 mysql_users、mysql_database_privileges、memory_info、information_columns、processlist 被拒绝；原 INFORMATION_SCHEMA 的四个公开来源可访问。角色启用后可查询角色获授视图。

普通用户 SHOW PROCESSLIST 能看到 root 会话。基线源码中的相同采集器和 SHOW 路径已经缺少 PROCESS/owner 过滤；此项记录为既有缺陷，本轮迁移不改变其语义。不能把这项检查记为通过。

回归新增在现有 [check_privilege.test](../../tools/deploy/mysql_test/test_suite/view/t/check_privilege.test) 中，没有另建 tools 下的测试框架。执行使用 `/u01/obclient/bin/mysqltest`，连接上述独立实例，临时 prologue 只提供测试框架的地址/端口变量。复验日志为 `ok`。

原始证据保存在本机 `/data/1/nijia.nj/table-function-implementation-notes/`：

- `baseline-view-execution.json`、`cutover-view-execution.json`：139 个视图逐项结果。
- `cutover-show-permissions.json`：93 项 SQL、结果、错误和判断。
- `cutover-locator-execution.json`：29 项定位/过滤结果。
- `cutover-mysqltest-check-privilege-record.log`、`cutover-mysqltest-check-privilege-verify.log`：实际 mysqltest 记录和复验。
- `cutover-consumer-cleanup.json`：清理后的元数据核对。

完成集成构建 `77d63463a` 的实例重启后，重新使用合入分支的 `view/check_privilege.test` 和预期文件执行 mysqltest，结果为 `ok`；再次执行全部 139 个系统视图，仍为 138 成功、1 个相同的基线错误，成功视图的列名及顺序均无变化。复验后检查测试库和用户已清理。

最终复验证据：`final-mysqltest-check-privilege-77d63463a.log`、`final-view-execution-77d63463a.log`、`final-view-execution-77d63463a.json`，均在上述证据目录。

补跑受迁移影响的两套既有 INFORMATION_SCHEMA 快照时发现并修复了两个回归：

- 视图列描述仍查找旧虚拟表 schema，遗漏函数绑定列的可空标记。消费者现直接读取绑定列信息；函数输出没有可插入的存储列默认值，因此 DEFAULT 显示 NULL，不为保留旧伪 schema 的隐式默认值新增框架字段。
- 系统函数把数值列的排序规则优先级标为 IMPLICIT，导致 `PROCESSLIST.HOST` 和 `V$OB_LOCKS.ID3` 的拼接结果变为 varbinary。输出描述现在复用现有列类型的优先级规则；新回归断言验证 IP/端口的 coercibility 分别为 2/5，拼接字符集为 utf8mb4。

修复后的集成版本 `40b6a9302` 在 22885 重启验收：两套快照各自实际 record 与独立 verify 均为 `ok`；139 个视图 DESC 全部成功且没有上述规则之外的差异。目录快照的系统对象从 384 个变为 241 个，减少的名称与 143 项 provider 迁移清单完全一致，没有新增目录对象或丢失真实系统表。创建测试表后的相应快照为 385/242，差值同样为 143。约束、索引、字符集、数据库和 FILES 查询与基线完全一致。系统视图定义中的来源改为函数调用。原快照的 INDEX_LENGTH 会随后台落盘变化，已与已有的行数、长度和时间字段一起归一化。

同一版本再次执行 `view/check_privilege` 为 `ok`，139 视图 LIMIT 1 仍为 138 成功及唯一相同的基线 4016。两实例新增的 admin 用户与 test1 测试库均已清理；原先存在的 test 数据库保持不变。

新增证据仍保存在上述本机目录：

- `all-view-description-before-fix.json`、`all-view-description-after-fix.json`：139 视图逐列基线对照及 64 个 DEFAULT 变化。
- `processlist-coercibility-before-fix.json`：两实例原始类型推导差异。
- `schema-snapshot-catalog-comparison.json`、`schema-snapshot-final-comparison.json`：143 项移除清单及其余查询逐项对照。
- `information-schema-record-40b6a9302.log`、`information-schema-verify-40b6a9302.log`、`information-schema-desc-record-40b6a9302.log`、`information-schema-desc-verify-40b6a9302.log`：两套快照记录和独立比较，全部为 ok。
- `final-mysqltest-check-privilege-40b6a9302.log`、`final-view-execution-40b6a9302.json`、`schema-snapshot-fixture-cleanup.json`：权限回归、全部视图执行与临时数据清理。

历史基线问题不作为迁移新增失败。

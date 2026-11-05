# 表函数迁移验证记录

日期：2026-10-17。状态：实现和本地验收完成，最终审查无未解决的 Blocker/Major。基线为交付前再次获取的最新 `origin/master`：`a8afc89e9bb3fd4db41cefcb77274a1fe35879cc`。最终生产候选 `40b6a9302` 的 release、SQL 和 observer 测试目标构建通过；`3abd404d9` 仅追加快照测试和记录，生产源码一致。依赖和 Cargo 使用现有本地缓存。远端完整 CI 的状态以 PR 检查为准。

## 新实例对比

基线和切换实例均使用 `memory_budget=2G`、`cpu_count=4`、`datafile_size=512M`、`log_disk_size=2G`，在独立空目录启动。按迁移清单的全部 143 个旧 ID 查询，而非按名称前缀估算。

| 指标 | 基线 | 完整切换后 |
| --- | ---: | ---: |
| 旧 VT 的 `__all_table` 行 | 143 | 0 |
| 旧 VT 的 `__all_column` 行 | 1664 | 0 |
| 旧 VT 的 `__all_table_history` 行 | 143 | 0 |
| 旧 VT 的 `__all_column_history` 行 | 1664 | 0 |
| 旧 VT 的 `__all_ddl_operation` 行 | 143 | 0 |
| 五类元数据合计 | 3757 | 0 |
| 可绑定的旧 VT 名称 | 143 | 0 |
| 系统视图 | 139 | 139 |
| schema 分配器 `mem_used` 快照 | 1,240,016 B | 1,019,904 B |
| schema 分配器 `mem_total` 快照 | 2,079,744 B | 2,079,744 B |
| SSTable 实际已用宏块 | 8 MiB | 8 MiB |
| SSTable 文件预分配 | 512 MiB | 512 MiB |
| redo 文件 | 64 MiB | 64 MiB |

schema 已用内存快照减少 220,112 B，预留内存没有变化；管理器数量和启动查询会影响快照，不将其当作精确的进程 RSS 净收益。LocalStore 数据文件为 311,296 B 与 307,200 B，相差一个页，包含真实本地系统数据，不能归因于 VT 元数据消除。日志文件不用于计算收益。已消除持久化记录不等于在当前小实例中减少了 SSTable 宏块或预分配文件尺寸。

新注册信息也有成本：当前构建 `sizeof(TableFunctionDescriptor)=120`、`sizeof(TableFunctionColumn)=48`，143 个固定函数及 1664 个列描述共 97,032 B，另有定位数组、静态初始化 guard 和字符串。生成定义中去重后的字符串字面量共 27,559 B，链接器还可能与程序其他字符串合并；这不是额外堆分配量。四个通用入口另有自己的少量定义。注册表持有程序生命周期内的定义指针，不创建持久化函数或表 schema。

## 功能和消费者验证

- 143 个函数逐一 `SELECT * ... LIMIT 0`：列名、顺序、协议类型与旧表一致；逐一 `DESCRIBE TABLE FUNCTION name`：列名和数量与 SELECT 一致。协议列长度、精度、字符集等也对齐；不再携带旧虚拟表主键/分区键的存储标记。
- 143 个函数逐一有限扫描：137 项成功。其余六项在两端返回相同错误：`plan_stat`、`table_mgr`、`px_target_monitor` 的既有 4016，未创建的过程和触发器，以及无效的 `tx_data(0)` 参数。137 个可成功扫描的提供方也通过不读取输出列的 `SELECT 1 ... LIMIT 1`。
- `SHOW TABLE FUNCTIONS` 返回 147 个签名。完整 SQL 回归覆盖名称和别名、单/多/嵌套 UNNEST、空值和补 NULL、LEFT/CROSS 关联、GENERATOR 表达式/占位符/用户变量及求值次数、JSON_TABLE 路径/嵌套/序号/EXISTS/错误策略、描述不执行函数、HYBRID_SEARCH 查询展开与结构参数常量折叠、预处理限制和不可缓存场景。
- 139 个视图逐一 `LIMIT 1`：两端均 138 项成功，唯一错误 `V$OB_PX_TARGET_MONITOR` 的 4016 存在于基线。139 个视图的 DESC 列名、类型、可空性、Key、Extra 与基线一致；64 个函数输出列的 DEFAULT 由原伪 schema 的空白默认值改为 NULL。
- 两套 INFORMATION_SCHEMA 目录快照均实际 record 并独立 verify。目录准确减少清单中的 143 个 VT，没有新增对象或丢失真实系统表。约束、索引、字符集、数据库及 FILES 查询与基线一致。
- SHOW、DDL、普通用户权限和对象可见性检查共 93 项，其中 92 项通过；29 项定位/过滤检查通过。普通用户 SHOW PROCESSLIST 可见其他会话是基线既有缺陷，不记为通过。完整清单与快照说明见[消费者验收](./table-function-consumer-validation.md)。
- 四连接共 1500 次操作验证 GENERATOR 状态隔离、LocalStore 有限扫描、系统配置有限扫描、关联 LEFT JOIN 重扫及无效参数失败路径。长 GENERATOR 查询可取消，返回 1317 后连接继续使用成功。取消响应约 2.5 ms，仅代表该次本地测量。
- 独立实例经过重启和回归中的落盘/合并后，真实用户表记录仍存在，LocalStore 提供方可查询。最终重查五类旧 VT 元数据仍全部为 0，143 个旧名字均无法绑定，系统视图仍为 139 个。11 个真实 LocalStore 定义保留。

## 编译与自动化测试

生产构建使用仓库 `build.sh release --init`，随后在 `build_release` 执行 `make -j80 seekdb sql_tests observer_tests`，设置 `CARGO_NET_OFFLINE=true`。解析器在 CMake 配置阶段重新生成，最终运行二进制版本为 `40b6a9302`。

| 测试 | 结果 |
| --- | --- |
| `sql_tests --gtest_filter='PlComposite*:TableFunction*'` | 18 项通过：9 项值编码、3 项真实打包输出、5 项集合扫描、1 项重载注册 |
| `observer_tests --gtest_filter='PlComposite*'` | 2 项通过：实际元数据转换与普通文本/二进制写出 |
| `unittest/share/inner_table/test_table_function_codegen.py` | 7 项通过 |
| `mysql_test/t/table_functions.test` | comparison 模式通过；最终修复未重录预期结果 |
| `mysql_test/test_suite/view/t/check_privilege.test` | 最终候选比较通过 |
| `mysql_test/t/information_schema.test` | record 和独立 comparison 均通过 |
| `mysql_test/test_suite/information_schema/t/information_schema_desc.test` | record 和独立 comparison 均通过 |
| `mysql_test/test_suite/array/t/array_ddl_mysql.test` | 既有预期比较通过 |
| `mysql_test/test_suite/array/t/array_arith_op_mysql.test` | 既有预期比较通过 |

mysqltest 使用 `/u01/obclient/bin/mysqltest` 及仓库 `tools/deploy/` 下既有测试体系；本地 prologue 只提供实例地址、端口和空测试密码，临时用户和表在完成后清理。源码清单、CMake 清单及单测模块检查通过。`.result` 保留 mysqltest 原生 TSV 的空末字段；源码、测试脚本和文档的 `git diff --check` 通过。

## PL 输出验证边界

普通与打包输出的文本/二进制路径均调用实际生产接口验证，打包测试进入 Rust 编码器并检查协议字节；记录成员名由实际 PL 编译类型表提供。验证包括 SQL NULL、内部 JSON null、空集合、记录、嵌套集合、转义、LOB 服务错误和 metadata 一致性，逻辑值在最终输出前保持 PL 类型。

当前 master 的公开 MySQL 语法无法声明这些 PL 复合返回值，也没有带键关联数组的存储实现。这里验证的是实际运行时及协议接口，没有声称已经执行此类用户 SQL 的网络端到端测试。非 NULL 带键集合明确返回不支持；本次不新增 PL 语言功能或虚构缺失的运行时。详见[输出设计](../design/pl-composite-client-output.md)。

## 审查与修复

最终代码审查发现 JSON_TABLE 关联重扫重复求值，已由同一个修复实现者处理并复验。修复前，三次关联调用产生五次求值；修复后单元素、多元素和空文档 LEFT JOIN 三种情况均与 master 的结果一致，且求值次数均为三。完整 table_functions mysqltest 在最终候选上直接比较通过。

消费者快照还发现并修复了视图可空性读取旧 schema，以及数值列 coercibility 导致拼接结果变成 varbinary 的问题；回归分别核对全部视图 DESC 和数值/字符串混合表达式。其他实现期修复包括 PL datum 缓冲区、typed NULL、嵌套数组 LOB 标记和普通算术参数解析。最终源码审查和快照证据增量核对没有未解决的 Blocker/Major。

## 原始证据

本机 `/data/1/nijia.nj/table-function-implementation-notes/` 保留原始日志；迁移清单、测试源、预期结果和本记录随 PR 提交。

- 基线/新实例：`baseline-instance.json`、`after-instance.json`；最终复查：`final-instance.json`、`final-restart-proof.json`。内存和磁盘对比仅取初始同配置快照，不混用后续 DDL 回归后的文件大小。
- 完整构建与单测：`final-candidate-configure.log`、`final-candidate-build.log`、`final-sql-units.log`、`final-observer-units.log`。
- 全量提供方：`provider-validation.json`、`provider-wire-metadata.json`、`review-zero-projection.json`；资源检查：`lifecycle-validation.json`。
- SQL 与审查修复：`core-review-mysqltest-verify.log`、`json-rescan-fixed-verification.json`、`array-ddl-regression.log`、`array-arith-regression.log`、`final-review.md`。
- 协议：`tf5-protocol/result.md`；消费者、快照及清理日志索引见[消费者验收记录](./table-function-consumer-validation.md)。

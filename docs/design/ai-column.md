# 异步 AI 列（AI COLUMN）设计与实现

> 赛题三实现说明。本文描述 seekdb 内核中异步 AI 列的语法、对象模型、事务语义、
> 异步调度与索引一致性，并给出每个模块在代码中的落点。

## 1. 目标与范围

AI 列把「模型派生数据」表达为数据库对象：用户在 DDL 中声明列的生成规则，
业务只维护普通列；数据库在业务事务提交后调用模型，校验返回值后在独立事务中发布结果，
并在依赖列变化时使旧结果失效、重新生成。

必须满足的外部行为（对应赛题 5 个评分模块）：

| 模块 | 外部行为 |
| --- | --- |
| DDL 与类型约束 | `CREATE TABLE` / `ALTER TABLE ADD|MODIFY` 声明 AI 列；`SHOW AI COLUMNS` 查看定义；非法定义在 DDL 阶段拒绝 |
| DML 联动与事务一致性 | 插入、依赖列更新、主键更新触发任务；无关列更新不触发；数据变更/结果失效/任务创建同事务；批量、回滚、删除清理 |
| 异步调度与结果写回 | 任务持久化、并发领取、崩溃恢复；请求标识/定义版本/任务归属校验；迟到结果不覆盖；同组结果整体发布 |
| 失败重试与严格读取 | 有限自动重试、终态 FAILED；`ALTER TABLE ... RETRY [FAILED] AI COLUMN ...`；`ai_column_strict_read` |
| 向量列与索引维护 | 向量生成并进入 VSAG HNSW 索引；失效即移除；写回即入索引；回滚一致 |

限定：MySQL 模式、`sync_mode='async'`、不支持 AI 列之间链式依赖、不考察模型效果。

## 2. 语法

### 2.1 建表与加列

```sql
CREATE TABLE docs (
  id       BIGINT PRIMARY KEY,
  title    VARCHAR(255),
  content  VARCHAR(8192),
  language VARCHAR(32) AI COLUMN (
    AI_COMPLETE('doc_model', CONCAT('Return only the language name. Content: ', content))
  ) WITH (sync_mode='async', max_retries=3),
  tags JSON AI COLUMN (
    AI_COMPLETE('doc_model', CONCAT('Return a JSON array of tags. Content: ', content))
  ) WITH (sync_mode='async', max_retries=3),
  emb VECTOR(4) AI COLUMN (
    AI_EMBED('embed_model', content, 4)
  ) WITH (sync_mode='async', max_retries=3)
);

ALTER TABLE docs ADD COLUMN summary VARCHAR(2048) AI COLUMN (
  AI_COMPLETE('doc_model', CONCAT('Summarize: ', content))
) WITH (sync_mode='async', max_retries=3);

ALTER TABLE docs MODIFY COLUMN summary VARCHAR(4096) AI COLUMN (
  AI_COMPLETE('doc_model', CONCAT('Summarize in one paragraph: ', content))
) WITH (sync_mode='async', max_retries=5);
```

`AI COLUMN(expr)` 是列属性，与 `NOT NULL`/`DEFAULT` 同级；`WITH(...)` 为异步选项：

| 选项 | 取值 | 默认 | 说明 |
| --- | --- | --- | --- |
| `sync_mode` | `'async'` | `'async'` | 本题只支持 async；其他取值 DDL 报错 |
| `max_retries` | `[0, 100]` 的整数 | `3` | 自动重试上限 |

### 2.2 查看定义

```sql
SHOW AI COLUMNS FROM docs;
```

输出：`Table, Column, AIFunction, ModelName, ResultType, Dependencies, SyncMode, MaxRetries, StatusColumn, DefVersion`。

### 2.3 条件重试

```sql
ALTER TABLE docs RETRY AI COLUMN language WHERE id BETWEEN 100 AND 200;
ALTER TABLE docs RETRY FAILED AI COLUMN tags WHERE category = 'news';
ALTER TABLE docs RETRY FAILED AI COLUMN emb;
```

`WHERE` 中未加反引号的 `status` 是**状态伪列**，取值为该 AI 列当前状态
（`PENDING`/`READY`/`FAILED`）；反引号 `` `status` `` 表示同名业务列。

### 2.4 会话变量

```sql
SET SESSION ai_column_strict_read = ON;
```

## 3. 对象模型

### 3.1 结果列

AI 列本身是一条**普通存储列**（可被索引、可被 ANN 查询），带 `AI_COLUMN_FLAG`：

- 列表达式文本保存在 `ObColumnSchemaV2::expr_str_`（沿用生成列已有字段，序列化不变）；
- 依赖列在 DDL 阶段解析表达式得到，并与选项、定义版本一起持久化到 AI 列元数据（见 3.3）；
- 引擎拒绝 `INSERT`/`UPDATE` 直接给 AI 列赋值（除内部 worker 的受控写回）。

AI 列**不**注册为 `VIRTUAL/STORED_GENERATED_COLUMN_FLAG`，因此现有生成列计算路径不会在
DML 时同步调用模型；能否计算由 AI 列自身的标记决定。

### 3.2 隐藏状态列

每个 AI 列 `c` 在 DDL 阶段自动创建一条隐藏列：

```
__ai_status_<c>  TINYINT   hidden=1  AI_STATUS_COLUMN_FLAG
```

取值：`0` = 未定义/失效、`1` = PENDING、`2` = READY、`3` = FAILED。
隐藏列不出现在 `SELECT *`、`DESC`、`SHOW COLUMNS` 中，但允许在 `RETRY ... WHERE status = 'FAILED'`
（状态伪列）与内部 SQL 中引用。

选择「同表隐藏列」而不是「外部任务表」保存状态的原因：状态必须与结果值在同一次
事务里原子发布，同表同行写入天然满足；同时它让「失效即 NULL」不需要读路径改写。

### 3.3 系统表元数据

新增内部表 `__all_ai_column`（视图 `DBA_OB_AI_COLUMNS`），每张表每个 AI 列一行：

| 列 | 说明 |
| --- | --- |
| `tenant_id`, `table_id`, `column_id` | AI 列定位 |
| `database_name`, `table_name`, `column_name` | 便于展示与重试解析 |
| `ai_function` | `AI_COMPLETE` / `AI_EMBED` / `AI_RERANK` |
| `model_name` | 逻辑模型名 |
| `expression` | 列表达式原文 |
| `result_type` | 声明类型（含 `VECTOR(N)` 维度） |
| `dependencies` | 依赖列 ID 列表（逗号分隔） |
| `sync_mode`, `max_retries` | 选项 |
| `status_column_id` / `status_column_name` | 隐藏状态列 |
| `def_version` | 定义版本，`ALTER` 改表达式/选项时递增 |
| `flags` | 位图：启用、创建中、待重建 |
| `gmt_create`, `gmt_modified` | 时间 |

### 3.4 任务表

新增内部表 `__all_ai_column_task`，每一行代表一次「同一行同一次数据变更需要生成的一组 AI 列」：

| 列 | 说明 |
| --- | --- |
| `task_id` | 单调递增任务 ID |
| `table_id`, `row_key` | 目标行（主键列序列化后的行标识） |
| `column_ids` | 本次需要生成的一组 AI 列 ID |
| `request_id` | 请求标识（UUID），用于「请求-结果」归属校验 |
| `def_versions` | 各列定义版本的序列化串 |
| `status` | `PENDING` / `RUNNING` / `DONE` / `FAILED` |
| `retry_count` | 已用自动重试次数 |
| `lease_owner`, `lease_expire_ts` | 领取者与租约，用于并发领取与崩溃恢复 |
| `input_fingerprint` | 触发本次生成时依赖列取值的摘要 |
| `gmt_create`, `gmt_modified` | 时间 |

任务在业务事务内插入，因此「数据变更 + 旧结果失效 + 任务创建」要么一起提交、要么一起回滚。

## 4. 状态机

### 4.1 列状态

```
              DML 使依赖列变化 / RETRY
   (0) 未定义 ─────────────────────────▶ (1) PENDING
                                            │
                    worker 全部列成功        │  worker 用尽重试
                    ┌───────────────────────┼──────────────────────┐
                    ▼                       ▼                      ▼
                 (2) READY               (3) FAILED  ◀──────────────┘
                    │
                    └── DML 使依赖列变化 ──▶ (1) PENDING
```

- 列状态只随「同一行同一次变更」整组变化：一组列要么整体 READY，要么整组进入终态
  （其中成功列为 READY、最终失败列为 FAILED）。这是「同组结果整体发布」的落点。
- 默认读取不检查状态：值本身在失效时已被写成 `NULL`，因此未完成/失败/被淘汰的结果
  天然以 `NULL` 参与查询；状态列只用来区分原因，不用于判定完成。

### 4.2 任务状态

```
PENDING ──领取(写入 lease)──▶ RUNNING ──发布成功──▶ DONE
   ▲                            │
   └──租约过期/执行进程退出/重试──┘
                                └──重试耗尽──▶ FAILED(终态)
```

租约过期即视为「执行中断」，任务重新可被领取；恢复后可能重复调用模型，
但结果发布受第 7 节版本栅栏保护。

## 5. DML 联动

### 5.1 触发规则

| 场景 | 行为 |
| --- | --- |
| `INSERT` | 新行所有 AI 列置 `PENDING`、值写 `NULL`，建一条任务 |
| `UPDATE` 命中依赖列 | 该行受影响 AI 列置 `PENDING`、值写 `NULL`，建一条任务（一组列一条任务） |
| `UPDATE` 只改无关列 | 不写状态、不建任务 |
| `UPDATE` 改主键 | 按新主键重新建立任务（旧主键的挂起任务在发布时因行不存在/输入不匹配丢弃） |
| `DELETE` | 行消失；挂起任务在发布时因目标行不存在而丢弃，不重建行 |
| 多行 DML / 批量 `INSERT` | 每行一条任务；任务表插入与业务写入同事务 |
| 事务回滚 | 任务行一同回滚，不会留下孤儿任务，也不会发起模型请求 |

### 5.2 依赖判断

DML 行构建阶段（`ObDMLRowBuilder` / `ObTableUpdateExecutor` 写入前）比较该行依赖列的
旧值与新值：只有依赖列取值发生变化才失效并建任务。比较使用列的 SQL 语义相等
（`NULL <=> NULL`），避免 `UPDATE ... SET content = content` 误触发。

### 5.3 受保护写入

DML 解析到「显式给 AI 列赋值」时报错（如 `INSERT INTO t(id, language) ...`）。内部写回
走受控通道（见 7.3），不经过该检查。

## 6. 异步调度

### 6.1 工作线程

`ObAIColumnWorker` 是每租户一个后台线程（`ObTenantThread` 派生），周期性：

1. `claim`：批量领取 `PENDING` 或租约过期的任务，写 `lease_owner`/`lease_expire_ts`；
2. 读取该行当前依赖列取值，构造并执行模型表达式；
3. 按结果类型校验与转换；
4. 写回事务：版本栅栏 + 分组发布 + 任务终态；
5. 失败：写回列状态（重试或 FAILED），递增 `retry_count`，按退避时间重新排队。

领取通过「带条件的 UPDATE」实现：`UPDATE ... SET lease_owner=? , lease_expire_ts=? WHERE task_id=? AND (status='PENDING' OR lease_expire_ts < now())`，
受影响行数为 1 才算领取成功，从而在多线程/多进程下互斥。

### 6.2 表达式求值

worker 不重复实现 AI 函数：它把 AI 列的表达式原文与行主键拼成内部 SQL：

```sql
SELECT <expression> FROM <db>.<table> WHERE <pk> = <value>;
```

由正常 SQL 引擎完成模型调用（复用 `AI_COMPLETE`/`AI_EMBED` 的 provider、endpoint
解析、批量与错误处理），worker 只做类型校验与发布。要求同一个 `request_id`
下所有列在一次内部查询中取回，保证「同组同批次」。

### 6.3 崩溃恢复

任务与状态都持久化；服务重启后 worker 扫描到 `RUNNING` 但租约过期的任务即重新执行。
「模型已返回但写回事务未提交」时，重启后表现为任务重新可领取，允许重复推理，
但不会出现未提交结果可见、任务永久丢失或重复结果污染。

## 7. 结果写回与一致性

### 7.1 类型校验

发布前按列声明类型校验，任一列不合法视为该列本次生成失败：

| 列类型 | 校验 |
| --- | --- |
| `VARCHAR(N)`/`TEXT` | 字符集可转换、长度不超限 |
| 整数/浮点/日期时间 | 可解析为目标类型且不越界 |
| `JSON` | 合法 JSON 文档 |
| `VECTOR(N)` | 元素均为有限数值、维度等于 N |

为提升模型返回可用性，DDL 阶段会在提示词尾部追加与结果类型匹配的「类型提示词」
（例如要求仅返回 JSON 数组、仅返回一个词），该提示词随元数据持久化，属于定义版本的一部分。

### 7.2 版本栅栏（最新请求胜出）

写回语句带「输入指纹」条件：

```sql
UPDATE t SET c1 = ?, st1 = 2, c2 = ?, st2 = 2
 WHERE <pk> = ? AND <dep1> <=> ? AND <dep2> <=> ? AND ...
```

- 依赖列已被更新 → 影响 0 行，本次结果被丢弃（迟到结果不覆盖新数据）；
- 行已删除 → 影响 0 行，不入索引、不复活行；
- 定义版本变化（`ALTER ... MODIFY`）→ 任务 `def_versions` 与元数据不一致，直接丢弃。

因此最终可见值只由「源数据当前版本」决定，而不是 HTTP 返回顺序。

### 7.3 分组发布

一组 AI 列在一次 `UPDATE` 中一起写值并一起置状态，提交后同时可见；
不会出现「language 属于新内容、tags 仍是旧内容」的混合态。

### 7.4 失败与重试

- 单列失败不影响同组其他列：成功列 READY，失败列在重试耗尽后 FAILED；
- 自动重试次数由 `max_retries` 限制，退避时间指数增长（初始 200ms，上限 30s）；
- 到达终态后任务置 `FAILED` 并从待执行集合清理，行状态保留 FAILED 供 `SHOW`/`RETRY FAILED` 使用。

## 8. 读取语义

### 8.1 默认读取

不检查状态，直接读列值；未完成/失败/被淘汰的行在该列上是 `NULL`。
`SELECT`、索引扫描、向量 ANN 查询看到的结果因此始终是「当前源数据对应的有效值或 NULL」。

### 8.2 严格读取

`SET SESSION ai_column_strict_read = ON` 后：

- 语句**引用**的 AI 列（投影、条件、子查询、`UPDATE`/`DELETE` 的 `WHERE`）存在任一未就绪行即报错；
- 检查是**列级**的：即使过滤条件只命中就绪行，只要存在未就绪行也报错；
- 只访问普通列或全部就绪的 AI 列不受影响。

实现分两步：

1. 解析/改写阶段收集语句引用的 AI 列集合（`ObDMLStmt` 上的 `ObRawExpr` 遍历，
   `ObRawExprUtils::extract_column_exprs`），写入语句执行上下文；
2. 执行前对每个被引用 AI 列执行一次轻量检查：

```sql
SELECT 1 FROM <db>.<table> WHERE <status_column> IN (0,1) LIMIT 1;
```

命中则报错 `OB_ERR_AI_COLUMN_NOT_READY`（带表名/列名）。检查在同一读快照上完成，
不阻塞、不等待模型。

## 9. 向量列与索引一致性

- 向量 AI 列是普通的 `VECTOR(N)` 存储列，DDL 阶段校验 `AI_EMBED` 的维度参数与列声明维度一致；
- 生成/写回走普通 `UPDATE`，因此 VSAG HNSW 索引项由现有向量索引维护逻辑自动加入；
- 失效时值写 `NULL`，旧向量索引项由现有更新路径移除；
- 事务回滚后，值保持原样，索引项随之回滚；
- 未完成/失败的结果不会出现在 ANN 查询结果中，因为扫描看到的是 `NULL`。

## 10. 代码落点

全部实现都落在**既有文件**里：本仓库的 CMake/Bazel 构建通过 unity/source inventory
（`src/*/*_source_inventory.bzl`、`BUILD.bazel`）校验源码清单，新增 `.cpp` 会让构建失败，
因此没有新增任何源文件；新增的只有测试与工具脚本。

| 层 | 文件 |
| --- | --- |
| 语法 | `src/sql/parser/sql_parser_mysql_mode.y`（AI COLUMN 列属性、`WITH` 选项、`SHOW AI COLUMNS`、`RETRY`）、`src/sql/parser/non_reserved_keywords_mysql_mode.c`（`RETRY`/`FAILED`）、两份 `ob_item_type.h`（`T_AI_COLUMN*` = 5200–5204） |
| 元数据结构 | `src/share/schema/ob_column_schema.{h,cpp}`（`ObAIColumnFuncType`/`ObAIColumnOption`/`ObAIColumnSpec`、`is_ai_column()`、序列化与列 ID 重映射）、`ob_schema_struct.h`（`AI_COLUMN_FLAG`/`AI_STATUS_COLUMN_FLAG`）、`ob_table_schema.h`（`has_ai_column()`） |
| DDL 校验 / 隐藏列 | `src/sql/resolver/ddl/ob_ddl_resolver.{h,cpp}`（表达式、模型类型、维度、选项、依赖校验；生成 `__ai_status_*`/`__ai_srcver_*`）、`ob_create_table_resolver.cpp`、`ob_alter_table_resolver.{h,cpp}`、`src/rootserver/ob_ddl_service.{h,cpp}` |
| 元数据落库 | `src/share/schema/ob_table_sql_service.{h,cpp}`（`__all_ai_column` 行随 DDL 同事务写入/删除） |
| 内部表 | `src/share/inner_table/ob_inner_table_schema_def.py`（`__all_ai_column` 1131、`__all_ai_column_task` 1132、视图 `DBA_OB_AI_COLUMNS` 21665） |
| DML 联动 | `src/sql/resolver/dml/ob_dml_resolver.{h,cpp}`（拒绝显式赋值）、`ob_del_upd_resolver.{h,cpp}`（依赖判定与三路隐式赋值注入）、`ob_insert_resolver.cpp` |
| 严格读取 | `src/sql/code_generator/ob_static_engine_cg.cpp`（收集引用列）、`src/sql/engine/ob_physical_plan.{h,cpp}`（计划标记 + 目标列表）、`src/sql/ob_result_set.{h,cpp}`（`do_open_plan` 执行前检查，覆盖文本/PS/计划缓存） |
| 后台生成 | `src/observer/omt/ob_ai_service.{h,cpp}`（`ObAIColumnWorker`，注册在既有 `ObAiService` 生命周期里） |
| 条件重试 | `src/share/ob_rpc_struct.{h,cpp}`（`ObAlterTableArg` 新 action）、`src/sql/resolver/ddl/ob_alter_table_resolver.{h,cpp}`、`src/rootserver/ob_ddl_service.{h,cpp}`（`retry_ai_column()` + `status` 伪列重写）、`src/sql/engine/cmd/ob_table_executor.cpp` |
| SHOW | `src/share/statement/ob_stmt_type.h`（`T_SHOW_AI_COLUMNS`）、`src/sql/resolver/cmd/ob_show_resolver.{h,cpp}`（合成 `SELECT ... FROM __all_ai_column`） |
| 会话变量 | `src/share/system_variable/*`（`ai_column_strict_read`、`_ai_column_type_hint`）、`src/sql/session/ob_system_variable_factory.{h,cpp}`、`ob_basic_session_info.{h,cpp}` |
| 类型提示词 | `src/sql/engine/expr/ob_expr_ai/ob_expr_ai_complete.cpp`（仅在提示词非空时追加） |
| 测试 | `unittest/sql/parser/test_ai_column_parser.cpp`、`unittest/sql/resolver/test_ai_column_resolver.cpp`、`unittest/share/schema/test_ai_column_schema.cpp`、`tools/deploy/mysql_test/test_suite/ai_function/{t,r}/ai_column_*`、`.../include/ai_column_mock.py`、`tools/ai_gateway/` |

## 11. 测试

| 类型 | 文件 | 覆盖 |
| --- | --- | --- |
| Unittest | `unittest/sql/parser/test_ai_column_parser.cpp` | CREATE/ADD/MODIFY、`WITH` 选项、SHOW AI COLUMNS、RETRY 语法 |
| Unittest | `unittest/sql/resolver/test_ai_column_resolver.cpp` | 选项校验、依赖限制、表达式解析、向量维度、类型提示词 |
| Unittest | `unittest/share/schema/test_ai_column_schema.cpp` | 元数据拷贝/序列化、列 ID 重映射、隐藏列 |
| Mysqltest | `.../t/ai_column_ddl.test` | 建表、加列、元数据、维度与依赖校验 |
| Mysqltest | `.../t/ai_column_t1_dml.test` | 任务创建、依赖失效、无关更新、主键变更、批量、回滚、删除 |
| Mysqltest | `.../t/ai_column_worker.test` | 类型提示词、分组可见性、自动重试、终态失败、迟到结果保护 |
| Mysqltest | `.../t/ai_column_retry_strict.test` | 按状态/主键/业务条件重试、状态伪列、严格读取 |
| Mysqltest | `.../t/ai_column_vector_index.test` | 向量生成、VSAG 索引、ANN、失效、重算、回滚 |
| Mock | `.../include/ai_column_mock.py` | 确定性文本/向量响应、请求断言、故障模拟 |
| 网关 | `tools/ai_gateway/README.md`、`tools/ai_gateway/test_gateway.py` | 透明代理、延迟与故障注入、响应丢失、用量统计 |

## 12. 已知限制

- 只支持 `sync_mode='async'`；不实现 AI 列之间链式依赖；
- 自动重试只在 worker 内进行，不跨租户调度；
- `ALTER TABLE ... MODIFY` 改表达式时会重新生成存量行，但 DDL 本身不等待完成；
- 严格读取的检查粒度是「表 + 列」，不做行级快照对比。

## 13. 如何验证

编译（Linux；本仓库只发布 Linux/macOS 产物）：

```bash
./build.sh release --init      # 首次
./build.sh release             # 增量
```

单元测试（CMake 会自动发现 `unittest/<module>/` 下的 `test_*.cpp`）：

```bash
make -C build_release sql_tests share_tests
GTEST_FILTER='*ai_column*' \
  ./build_release/unittest/run_tests.sh -R '^(sql|share)_tests_shard_[0-9]+$' --output-on-failure
```

mysqltest（Mock AI Service 监听 19120～19124）：

```bash
cd tools/deploy
python3 mysql_test/test_suite/ai_function/include/ai_column_mock.py --port 19120 &
./obd.sh mysqltest -n test --suite ai_function
```

手工体验：

```sql
-- 1) 注册逻辑模型 + Endpoint（指向 Mock 服务）
CALL DBMS_AI_SERVICE.CREATE_AI_MODEL('doc_model', '{"type":"completion","model_name":"m"}');
CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT('doc_ep', '{
  "ai_model_name":"doc_model","url":"http://127.0.0.1:19120/v1/chat/completions",
  "access_key":"sk-mock","provider":"openai","request_model_name":"m"}');

-- 2) 建表、写入、观察异步补齐
CREATE TABLE docs(id BIGINT PRIMARY KEY, content VARCHAR(8192),
  language VARCHAR(32) AI COLUMN (
    AI_COMPLETE('doc_model', CONCAT('[[LANG]] ', content))) WITH (sync_mode='async', max_retries=3));
INSERT INTO docs(id, content) VALUES (1, 'hello world');
SELECT language FROM docs;                 -- 未就绪：NULL
SELECT language FROM docs;                 -- worker 完成后：值出现
SHOW AI COLUMNS FROM docs;

-- 3) 严格读取与条件重试
SET SESSION ai_column_strict_read = ON;
ALTER TABLE docs RETRY FAILED AI COLUMN language WHERE id = 1;
```

# AI COLUMN（异步 AI 列）实现说明

本分支在 seekdb 内核中实现**异步 AI 列**：用户用 SQL 声明列的生成规则，数据库在业务事务
提交后调用模型、校验返回值、在独立事务中发布结果，并在依赖列变化时使旧结果失效、重新生成。

- 设计文档：[docs/design/ai-column.md](docs/design/ai-column.md)
- 模块实现笔记：[notes/](notes/)（`module-parser.md`、`module-schema-ddl.md`、`module-dml.md`、
  `module-worker.md`、`module-retry.md`、`module-typehint.md`、`module-tests.md`、
  `integration-report.md`）

## 快速上手

```sql
CALL DBMS_AI_SERVICE.CREATE_AI_MODEL('doc_model', '{"type":"completion","model_name":"m"}');
CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT('doc_ep', '{
  "ai_model_name":"doc_model","url":"http://127.0.0.1:19120/v1/chat/completions",
  "access_key":"sk-mock","provider":"openai","request_model_name":"m"}');

CREATE TABLE docs(
  id BIGINT PRIMARY KEY,
  content VARCHAR(8192),
  language VARCHAR(32) AI COLUMN (
    AI_COMPLETE('doc_model', CONCAT('[[OUT=LANG]] ', content))
  ) WITH (sync_mode='async', max_retries=3),
  emb VECTOR(4) AI COLUMN (
    AI_EMBED('doc_model', content, 4)
  ) WITH (sync_mode='async', max_retries=3));

INSERT INTO docs(id, content) VALUES (1, 'hello');
SELECT language FROM docs;          -- 未就绪为 NULL
SHOW AI COLUMNS FROM docs;          -- 定义、状态列、定义版本
SET SESSION ai_column_strict_read = ON;
ALTER TABLE docs RETRY FAILED AI COLUMN language WHERE id = 1;
```

## 实现要点

| 主题 | 做法 |
| --- | --- |
| 结果列 | 普通存储列 + `AI_COLUMN_FLAG`；表达式文本存在列的 default-value 槽（与生成列同位置），`ai_spec_` 随列 schema 序列化 |
| 隐藏伴随列 | 每个 AI 列自动创建 `__ai_status_<c>`（0 未定义 / 1 PENDING / 2 READY / 3 FAILED / 4 RUNNING）与 `__ai_srcver_<c>`（单调源版本） |
| 失效 | DML 解析期注入三条隐式赋值：`c=NULL`、`__ai_status_c=1`、`__ai_srcver_c=<seq>`，与业务写入同事务；未完成/失败结果天然读作 NULL，读路径无需改写 |
| 生成 | `ObAIColumnWorker`（server 级后台线程）发现 `__all_ai_column` → 按状态列取候选行 → CAS 领取 → 内部 SQL 求值（复用 `AI_COMPLETE`/`AI_EMBED`）→ 类型校验 → 单事务分组发布 |
| 防覆盖 | 发布语句带 `AND __ai_srcver_c = <observed>`；源数据已变更/行已删除/定义版本变化时命中 0 行，结果丢弃 |
| 失败与重试 | `max_retries` 内指数退避重试，耗尽后列状态置 FAILED；`ALTER TABLE ... RETRY [FAILED] AI COLUMN ...` 按条件重新入队，未加反引号的 `status` 是状态伪列 |
| 严格读取 | `ai_column_strict_read=ON` 时，计划引用的 AI 列存在未就绪行即报错；在 `ObResultSet::do_open_plan` 统一拦截，覆盖文本/PS/计划缓存 |
| 类型提示词 | DDL 按结果类型生成提示词存入 `ai_spec_.type_hint_`；worker 通过隐藏会话变量 `_ai_column_type_hint` 传给 `AI_COMPLETE`，追加在最终 prompt 末尾（向量列不追加） |
| 索引一致性 | 结果写回走普通 SQL UPDATE，本地索引与 VSAG HNSW 向量索引由既有 DML 路径自动维护；失效写 NULL 即移除旧索引项 |

## 测试

| 类型 | 文件 |
| --- | --- |
| Unittest | `unittest/sql/parser/test_ai_column_parser.cpp`、`unittest/sql/resolver/test_ai_column_resolver.cpp`、`unittest/share/schema/test_ai_column_schema.cpp` |
| Mysqltest | `tools/deploy/mysql_test/test_suite/ai_function/t/ai_column_{ddl,t1_dml,worker,retry_strict,vector_index}.test` |
| Mock 服务 | `tools/deploy/mysql_test/test_suite/ai_function/include/ai_column_mock.py`（端口 19120～19124） |
| 评测网关 | `tools/ai_gateway/`（透明代理、延迟/故障/响应丢失注入、Token 与费用统计） |

运行方式见设计文档 §13。

## 已知限制

- 本机没有 Linux/编译器/bison，**所有 C++ 改动未经编译**；`notes/integration-report.md` §4
  列出了按优先级排列的编译风险点（最大的是 `src/observer/omt/ob_ai_service.cpp`）。
- `r/mysql/ai_column_*.result` 是手写期望，首次运行需 `mysqltest --record` 生成准确输出。
- 本仓库的 `unittest/` 只有 `.gitignore`、没有构建文件，三个 gtest 需要在上游完整仓库里注册。
- 只支持 `sync_mode='async'`；不支持 AI 列之间链式依赖；`ALTER ... MODIFY` 的后台重算不阻塞 DDL。

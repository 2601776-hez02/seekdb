# MiniVectorDB

轻量单机向量数据库：支持基础 SQL、L2 精确 TopK / ANN 向量检索、持久化存储、
事务与崩溃恢复。实现完全自研（约 3000 行 C++17，仅依赖标准库），核心存储、
查询执行、向量索引与事务逻辑均未使用现成数据库/索引库。

## 1. 语言与版本

- 语言：C++17（g++ 8+ 可编译；镜像内使用 Ubuntu 24.04 的 g++ 13）
- 依赖：无第三方库；构建产物为单静态链接二进制
- 运行形态：Linux x86_64 容器前台进程，stdin 接收 SQL、stdout 返回结果、
  日志写 stderr，不监听任何网络端口

## 2. 镜像构建与启动

```bash
docker build -t minivectordb:latest .
docker run --rm -i -v minivectordb-data:/data minivectordb:latest
```

- 数据目录：`/data`（可通过 `--data-dir <path>` 覆盖）
- 启动完成后向 stdout 输出一行 `{"type":"ready"}` 并刷新，随后逐行读取
  JSONL 请求（官方 STDIO_PROTOCOL.md，见根目录）

## 3. 进程协议（官方 JSONL 协议，已通过题包 protocol_check 全部检查）

- 握手：`{"type":"ready"}`（恢复完成后、读取任何请求之前主动输出）
- 请求（每行一个 JSON 对象）：
  - `{"id":N,"op":"execute","sql":"<SQL>","search_mode":"exact"|"ann"}`
  - `{"id":N,"op":"shutdown"}`
- 响应（每行一个 JSON 对象，原样回传 `id` 及其 JSON 类型）：
  - 查询：`{"id":N,"ok":true,"columns":[...],"rows":[[...],...]}`
  - DML：`{"id":N,"ok":true,"affected_rows":N}`
  - 其它成功：`{"id":N,"ok":true}`
  - 失败：`{"id":N,"ok":false,"error":{"code":"<语义名>","message":"..."}}`
- shutdown 响应后不再向 stdout 输出任何内容，并以退出码 0 结束；
  SIGTERM / SIGINT / stdin EOF 同样触发正常关闭流程（回滚未提交事务、落盘）
- 错误语义名：`syntax_error`、`unsupported_sql`、`table_not_found`、
  `column_not_found`、`duplicate_table`、`duplicate_key`、`duplicate_index`、
  `not_vector_column`、`dimension_mismatch`、`invalid_vector`、
  `value_overflow`、`string_too_long`、`txn_state`、`io_error`

## 4. SQL 支持范围

- DDL：`CREATE TABLE t (id BIGINT PRIMARY KEY, c VARCHAR(n), v VECTOR(d))`
  （一个 BIGINT 主键、每表至多一个向量列）；`CREATE VECTOR INDEX name ON t(v)`
- DML：多行 `INSERT ... VALUES`、`UPDATE ... SET ... WHERE`、`DELETE ... WHERE`
  （按主键或条件）；无 WHERE 时作用于全表
- 查询：列投影、`SELECT *`、比较运算符 `= < <= > >=`、`AND/OR/NOT`、
  `ORDER BY col [ASC|DESC]`（并列按主键升序）、`LIMIT`、
  `L2_DISTANCE(v, '[...]') AS alias`
- 事务：`BEGIN` / `COMMIT` / `ROLLBACK`；同一时刻最多一个显式事务；
  未显式 BEGIN 的修改自动提交；事务内可见自身修改；失败语句不留部分修改
- 不要求（返回约定错误）：NULL、JOIN、聚合、子查询、ALTER TABLE、主键更新、
  嵌套事务；`ALTER TABLE` 等返回 `unsupported_sql`
- 语义细节：VARCHAR 长度按 Unicode 字符计、比较按码点序；向量为固定维度
  float32，拒绝 NaN/Infinity/维度不符/float32 溢出；距离按存储后的 float32
  计算，返回欧氏距离；距离并列按主键升序

## 5. 操作示例

```sql
CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(32),
                   content VARCHAR(1024), embedding VECTOR(4));
INSERT INTO docs VALUES
  (1, 'database', '事务与恢复', '[1, 0, 0, 0]'),
  (2, 'database', '向量索引',   '[0, 1, 0, 0]'),
  (3, 'travel',   '旅行攻略',   '[0, 0, 1, 0]');
SELECT id, content, L2_DISTANCE(embedding, '[0.9, 0.1, 0, 0]') AS distance
FROM docs WHERE category = 'database' ORDER BY distance LIMIT 2;
CREATE VECTOR INDEX docs_vec ON docs(embedding);
--mode ann
SELECT id, L2_DISTANCE(embedding, '[0.9, 0.1, 0, 0]') AS distance
FROM docs ORDER BY distance LIMIT 2;
BEGIN;
UPDATE docs SET embedding = '[0.8, 0.2, 0, 0]' WHERE id = 1;
DELETE FROM docs WHERE id = 2;
ROLLBACK;
-- 正常关闭/强杀后重启：已提交数据保留，未提交事务不生效
```

## 6. 存储与恢复设计

### 6.1 记录布局

- `/data/catalog`：二进制目录（表名、列定义、主键/向量列位置、向量索引
  参数），原子替换（tmp + rename + 目录 fsync）
- `/data/wal.log`：WAL 记录 `[magic][len][crc32][payload]`；payload =
  `[type][txn_id][body]`；type：1=DDL(完整目录快照)、2=行写(表id+flags+pk+行)、
  3=COMMIT。每条提交 `fdatasync` 后才应答
- `/data/tables/<id>/run.<seq>.dat`：按主键有序的不可变 run 文件
  （头 8B 计数；条目 = `[pk][flags][len][行载荷]`，flags bit0=删除墓碑），
  mmap 读取 + 每 64 条一个稀疏采样索引
- 内存 MemTable（追加缓冲 + pk→offset 哈希）达到 64 MiB 时排序落盘为 run；
  run 数 > 16 时多路归并压缩；WAL ≥ 256 MiB 且无活跃事务时做检查点
  （全部落盘 → 截断 WAL）

### 6.2 事务与崩溃恢复

- 显式事务的修改写入每表 overlay（内存）；提交时先写 WAL(PUT×n+COMMIT) 并
  fsync，再并入 MemTable 并更新向量索引；回滚直接丢弃 overlay
- 单客户端、事务不交错，因此崩溃后只有 WAL 尾部最后一个事务可能未提交：
  恢复采用两遍扫描（内存有界）：第一遍定位最后一个完整 COMMIT 与首个损坏
  记录；第二遍把 COMMIT 之前的 PUT 流式重放进 MemTable（边放边落盘），
  COMMIT 之后与损坏尾部整体丢弃
- 语句原子性：先完整校验（类型/维度/长度/主键重复）再写 WAL，任何一步失败
  都不产生部分效果；大 UPDATE/DELETE 采用流式两遍执行——第一遍边扫描边把
  PUT 流式写入 WAL（出错即截断回滚），fsync COMMIT 后第二遍回读刚写的 WAL
  段并入 MemTable/索引（可按阈值落盘），全程内存有界，且崩溃时整条语句
  要么全生效要么全不生效
- UPDATE 遵循 MySQL 默认语义：值未变化的行不计入 affected_rows

### 6.3 向量检索

- 精确模式：扫描 + 距离堆，TopK = `(距离, 主键)` 升序前 K，过滤先于 TopK；
  距离用 float32 存储值计算，双精度累加；扫描路径按需解码（谓词引用列 +
  向量列，其余字段跳过拷贝）
- ANN：自实现 HNSW（L2），M=16 / efConstruction=200 / 搜索 ef 自适应 =
  max(1024, K×64, N/180)（N 为索引规模——召回随数据量衰减，需按规模放大
  ef 才能稳定保持 Recall@10 ≥ 0.95；1M×128d 实测 ef≈5500 → 0.95+）；
  构建为分批并行（工作线程对稳定图快照做检索，落链阶段串行，无并发读写）；
  建索引后增/删/改实时维护（更新 = 旧节点标记死亡 + 追加新节点）；
  过滤 TopK 走精确扫描（保证过滤组 Recall@10 = 1.0）
- 事务与索引一致：查询时 overlay 非空则回退精确扫描；提交后再并入索引
- 恢复：干净检查点（全部落盘 + WAL 截断）时把索引图序列化到
  `tables/<id>/index.bin`（带 CRC），干净重启直接加载（200K×64d 约 0.2s）；
  WAL 非空（崩溃）或文件缺失/损坏时从持久化记录全量重建（约 50s@200K），
  重建完成前不输出 READY

## 7. 已实现功能清单 / 已知限制

- 已实现：上述全部 SQL 子集；WAL+CRC 崩溃恢复；正常关闭回滚；
  重启数据一致；多表；Unicode/BIGINT 全范围；float32 语义；
  向量索引增删改维护；ann/exact 模式切换；内存受限下的落盘存储
- 已知限制：不实现 NULL/JOIN/聚合/子查询/ALTER；单客户端单事务；
  过滤 TopK 恒为精确扫描（换取召回保证）；显式事务的 overlay 常驻内存

## 8. 本地验证

- 公开题包 20 用例 95 条 SQL：20/20 通过（`tests/driver.py`）
- 健壮性（`tests/robust.py`，15 项）：kill -9 后已提交保留/未提交丢弃、
  大语句中途强杀的全有全无、半写 WAL 截断、NaN/Inf/溢出/维度错误、
  事务内索引一致性、未知列报错、同值更新不计、多列 ORDER BY+LIMIT 堆、
  大 DELETE/UPDATE 流式执行、SIGTERM 正常关闭、BIGINT 极值/转义/列序 INSERT、
  10 万行单语句、MemTable 墓碑压过已落盘 run：全部通过
- 崩溃模糊（`tests/fuzz_crash.py`，极小阈值 + 随机 kill，12 轮）：全部通过
- 索引重启：干净重启加载序列化索引图 0.23s（200K×64d），崩溃后重建正确
- 规模冒烟：200K×64d 写 64K 行/s（RSS 182MB）、建索引 33s（AVX2）、
  精确 TopK 15.4ms、ANN Recall 0.99；1M×128d 写 21.6K 行/s、建索引 402s（4 核）、
  自适应 ef 下 ANN Recall 0.97（39ms/查询）、过滤组 Recall 1.0（精确扫描）

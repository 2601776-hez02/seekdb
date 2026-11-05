# 文件即数据库（File SQL）

更新日期：2026-10-31。状态：`read_csv` / `read_json` / `read_ndjson` / `read_parquet` / `glob()` 已接入表函数框架，`enable_file_sql` 与 `secure_file_priv` 双重校验、类型推断、Parquet（含 Snappy）解码、文件变化检测和单元测试均已交付。

## 1. 目标与边界

用户不再需要建表或导入数据：`FROM read_csv('path')` 直接读取本地文件，列结构由文件内容推断，过滤、聚合、排序与 JOIN 全部由 seekdb SQL 引擎执行（复用表函数框架的 `ITableFunctionScan` 和原生执行计划），不使用任何外部查询引擎。

| 事项 | 约定 |
| --- | --- |
| 入口 | `read_csv(path [, option = value, ...])`、`read_json` / `read_ndjson`、`read_parquet(path)`、`glob(pattern)` |
| 输入 | 单进程本地文件，只读 |
| 开关 | `enable_file_sql`（SESSION/GLOBAL，默认 OFF）；开启后仍须通过 `secure_file_priv` |
| 多文件 | `glob()` 只做文件发现并返回一列 `file`；不做多文件自动合并 |
| 嵌套结构 | JSON 只接受扁平对象，嵌套对象/数组直接报错；Parquet 只接受扁平 schema |
| 推断范围 | CSV / JSONL 使用全量 Schema 推断，`sample_size` 仅支持 `-1` |
| 流式 | 扫描按块读取，只保留当前记录；推断阶段一次遍历、不物化文件 |

## 2. 绑定期与执行期的分工

绑定期（`ObDMLResolver::bind_file_table_function`）完成所有与文件内容相关的工作，执行期不再做任何推断：

1. 校验 `enable_file_sql`，解析命名参数，求出路径常量；
2. `realpath` 归一化后调用 `ObResolverUtils::check_secure_path` 做 `secure_file_priv` 校验；
3. 全量推断列名与类型，记录文件 `size` 与 `mtime`；
4. 通过 `resolve_function_table_column_item` 把推断结果登记为查询局部列，于是 `SELECT *`、别名、`DESCRIBE TABLE FUNCTION`、权限与依赖都复用现有关系绑定；
5. 把「格式 + 选项 + 路径 + 列定义 + 文件指纹」序列化成一段二进制快照，存入 `TableItem::file_sql_options_`，再由 `compile_file_table_options` 原样拷进执行计划。

执行期（`FileTableScan`）只做三件事：再次校验开关；比对文件指纹，文件在绑定后被改写则报错而不是返回错误结果；按投影列流式解码并转换为 `ObObj`。

`read_csv('a.csv')` 的第 1 个参数是唯一的执行期表达式，命名参数在绑定期消费；因此 `SELECT * FROM read_csv('a.csv')` 的计划结构与普通表函数扫描完全一致，`WHERE` / `ORDER BY` / `GROUP BY` / `HAVING` / `LIMIT` / `JOIN` 都走原生算子。

## 3. 类型推断

对每个取值先做确定性的字面判定，再按列做宽化：`NULL` → `BOOLEAN` → `BIGINT` → `DOUBLE` → `DATE` → `DATETIME` → `VARCHAR`。

| 规则 | 说明 |
| --- | --- |
| 整数/浮点 | `BIGINT` 要求整体落在 `int64` 内；超出或带小数点/指数即为 `DOUBLE` |
| 日期/时间 | 只接受 `YYYY-MM-DD`、`YYYY-MM-DD HH:MM:SS[.ffffff]`（`T` 分隔同样接受并归一化为空格）；非法日历日（如 `2023-02-29`）不会推断成 `DATE` |
| 布尔 | `true` / `false`（大小写不敏感） |
| 全 NULL 列 | 推断为 `null` 类型，值恒为 NULL |
| 宽化 | `BIGINT`+`DOUBLE`→`DOUBLE`；`DATE`+`DATETIME`→`DATETIME`；其余混合→`VARCHAR` |
| 空串 | 不等于 NULL；只有配置了 `nullstr` 且命中才算 NULL |
| 列名 | CSV 用表头（无表头时 `column0`、`column1`……），JSONL 按首次出现顺序取字段并集，重名自动加 `_1` 后缀 |

## 4. 命名参数

```
read_csv('a.csv', header = true, delim = ';', quote = '"', escape = '"',
         nullstr = 'NA', skip_rows = 1, sample_size = -1)
```

参数在语法上就是 `name = value` 表达式：解析器不需要改动，绑定期识别 `T_OP_EQ` 左侧的裸标识符或字符串字面量即可。未知参数、非法取值（如 `sample_size` 不等于 -1）和非字符串路径都会给出可读诊断。

## 5. 安全与稳定性

- 功能默认关闭；`enable_file_sql` 带 `INFLUENCE_PLAN`，改开关或改 `secure_file_priv` 都不会复用旧计划。
- 路径先 `realpath` 再校验，`..`、软链接和相对路径都不能逃出 `secure_file_priv`；`secure_file_priv` 为空时一律拒绝。
- 绑定期记录文件指纹，执行期不一致即报错。
- 单条记录上限 16 MiB、列数上限 1024、`glob()` 命中上限 65536，全部超出即报错而不是 OOM。
- 解析失败给出行号/字段数等定位信息（`line 2: ...`、`a data row has 1 fields but 2 columns were inferred`）。

## 6. 实现结构

| 文件 | 职责 |
| --- | --- |
| `src/sql/table_function/file_sql.h/.cpp` | 与 OceanBase 无关的读取核心：分块文件流、CSV 状态机、扁平 JSON 解析、类型推断、Parquet（Thrift compact 元数据、PLAIN/RLE/字典页、Snappy 解压）、glob、快照编解码 |
| `src/sql/table_function/file_table_function.cpp` | SQL 侧适配：绑定、快照生成、扫描算子到 `ObObj` 的转换 |
| `src/sql/table_function/table_function.cpp` | 注册 5 个内建表函数描述符 |
| `src/share/system_variable/ob_system_variable_init.json` | `enable_file_sql` 的定义（由 `gen_ob_sys_variables.py` 生成其它 7 个文件） |

把与引擎无关的读取核心独立出来的直接收益是它可以脱离 observer 编译和测试：`unittest/sql/engine/basic/test_file_scan_utils.cpp` 覆盖 CSV 选项与坏行诊断、JSONL 字段并集、快照编解码、文件变化检测，以及内置的 Parquet（Snappy + 字典编码 + 多 Row Group）用例。

## 7. 限制

- 不支持远程文件、多文件自动合并、嵌套结构、Parquet 的 INT96 / FIXED_LEN_BYTE_ARRAY / GZIP 压缩。
- CSV / JSONL 只有全量推断，`sample_size` 仅接受 -1。
- `COPY table TO path` 导出不在本次范围内。

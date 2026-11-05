# DuckDB 表函数入口与输出列名

调研日期：2026-10-16。主要证据为用户提供的本地源码 `/home/nijia.nj/nijia.nj/duckdb-main`（实际路径 `/data/1/nijia.nj/duckdb-main`）。该目录不带 Git 元数据，不能确认 commit 或发行版；以下代码行为只归属于此快照。在线 DuckDB 文档作为补充，不能用当前文档新增能力反推本地快照的能力。没有安装 DuckDB 或执行 SQL；例子根据源码及仓库已有测试推导。

## 结论

DuckDB 把很多功能统一成可以直接写在 `FROM` 中的表函数：`FROM range(...)`、`FROM unnest(...)`、`FROM json_each(...)`、`FROM read_json(...)`。这些入口不用再包装 `TABLE(...)`。它统一的是表函数的注册、绑定和输出列接口，并不要求所有函数具有相同的参数语义或列名规则。[官方 FROM 文档](https://duckdb.org/docs/current/sql/query_syntax/from)、[本地表函数 catalog 查找与重载绑定](/home/nijia.nj/nijia.nj/duckdb-main/src/planner/binder/tableref/bind_table_function.cpp:367)

对常规表函数，函数自己的 `bind` 回调生成有顺序的 `names`；通用 binder 处理调用处的列别名和空名兜底，将列加入查询的绑定上下文。外层 `SELECT` 再按投影决定最终结果列名。这提供了统一机制，而列名来源仍由各函数决定。[回调类型](/home/nijia.nj/nijia.nj/duckdb-main/src/include/duckdb/function/table_function.hpp:269)、[通用绑定流程](/home/nijia.nj/nijia.nj/duckdb-main/src/planner/binder/tableref/bind_table_function.cpp:244)、[SELECT 列名](/home/nijia.nj/nijia.nj/duckdb-main/src/planner/binder/query_node/bind_select_node.cpp:423)

## 常规表函数如何交付列名

核心 C++ 回调是：

```cpp
unique_ptr<FunctionData> bind(
    ClientContext &context,
    TableFunctionBindInput &input,
    vector<LogicalType> &return_types,
    vector<string> &names);
```

`names` 是有顺序的输出列名数组，`FunctionData` 是绑定后供执行阶段使用的数据。执行回调另行写入 `DataChunk`，不会靠执行产生的每行数据决定列名。C API 也提供对应的 `duckdb_bind_add_result_column(info, name, type)`。[C++ 接口](/home/nijia.nj/nijia.nj/duckdb-main/src/include/duckdb/function/table_function.hpp:269)、[执行回调与状态](/home/nijia.nj/nijia.nj/duckdb-main/src/include/duckdb/function/table_function.hpp:341)、[官方 C API](https://duckdb.org/docs/current/clients/c/table_functions)

本地 `BindTableFunctionInternal` 常规路径的顺序：

1. 调函数自己的 `bind`，取得 `return_names`。
2. 检查列名数组和输出列数组长度一致，且至少有一列。
3. 将 `AS t(a, b)` 提供的列别名逐位覆盖到 `return_names`。
4. 剩余空名称按位置补成 `C0`、`C1` 等；这不是给所有表函数强制取函数名。
5. 创建 `LogicalGet`，调用 `AddTableFunction`，形成可供外层查询引用的 `TableBinding`。

证据：[回调调用](/home/nijia.nj/nijia.nj/duckdb-main/src/planner/binder/tableref/bind_table_function.cpp:244)、[检查与别名/空名处理](/home/nijia.nj/nijia.nj/duckdb-main/src/planner/binder/tableref/bind_table_function.cpp:273)、[加入绑定上下文](/home/nijia.nj/nijia.nj/duckdb-main/src/planner/binder/tableref/bind_table_function.cpp:350)、[创建 TableBinding](/home/nijia.nj/nijia.nj/duckdb-main/src/planner/bind_context.cpp:653)。

表别名与列别名分别处理：只有 `AS t` 时，`t` 是绑定关系名，不会覆盖输出列名；`AS t(x)` 才会覆盖第一列名称。[表别名读取](/home/nijia.nj/nijia.nj/duckdb-main/src/planner/binder/tableref/bind_table_function.cpp:180)、[列别名覆盖](/home/nijia.nj/nijia.nj/duckdb-main/src/planner/binder/tableref/bind_table_function.cpp:280)

普通 `SELECT *` 从 `binding.names` 按顺序展开列引用；`BindSelectNode` 从投影表达式取名称，有显式 SELECT 别名时再使用该别名。[星号展开](/home/nijia.nj/nijia.nj/duckdb-main/src/planner/bind_context.cpp:486)、[投影列名](/home/nijia.nj/nijia.nj/duckdb-main/src/planner/binder/query_node/bind_select_node.cpp:423)

## 不同函数的列名从哪里来

| 写法 | 本地快照中的列名规则 | 证据 |
| --- | --- | --- |
| `FROM range(3)` | bind 固定添加 `range` | [RangeFunctionBind](/home/nijia.nj/nijia.nj/duckdb-main/src/function/table/range.cpp:59) |
| `FROM generate_series(1, 3)` | bind 固定添加 `generate_series` | [RangeFunctionBind](/home/nijia.nj/nijia.nj/duckdb-main/src/function/table/range.cpp:59) |
| `FROM unnest([1, 2])` | bind 固定添加 `unnest` | [UnnestBind](/home/nijia.nj/nijia.nj/duckdb-main/src/function/table/unnest.cpp:44) |
| `FROM json_each(json)`、`FROM json_tree(json)` | 固定八个普通列：`key`、`value`、`type`、`atom`、`id`、`parent`、`fullkey`、`path` | [JSONTableInOutBind](/home/nijia.nj/nijia.nj/duckdb-main/extension/json/json_functions/json_table_in_out.cpp:9) |
| `FROM read_json('users.json')` | 自动检测为 records 时，取检测所得结构的成员名；按 values 读取时默认单列 `json` | [JSONScan::AutoDetect](/home/nijia.nj/nijia.nj/duckdb-main/extension/json/json_functions/read_json.cpp:200) |
| `FROM read_json(..., columns = {...})` | 显式 `columns` 参数中结构成员的名称进入列名列表 | [columns 参数解析](/home/nijia.nj/nijia.nj/duckdb-main/extension/json/json_multi_file_info.cpp:76) |

`range`、`generate_series`、`FROM unnest`、`json_each`、`json_tree` 都注册为 `TableFunction`，在本地快照中使用 `in_out_function` 消费输入并产生输出。`range` 自己保存推进位置，每次至多生成一批值，无需先构造完整数组。函数名 `range` 不需要由通用 binder 专门识别放行。[range 注册](/home/nijia.nj/nijia.nj/duckdb-main/src/function/table/range.cpp:381)、[range 批次生成](/home/nijia.nj/nijia.nj/duckdb-main/src/function/table/range.cpp:134)、[unnest 注册](/home/nijia.nj/nijia.nj/duckdb-main/src/function/table/unnest.cpp:80)、[JSON 注册](/home/nijia.nj/nijia.nj/duckdb-main/extension/json/json_functions/json_table_in_out.cpp:379)

`json_each` 的 JSON 对象 key 是行数据，并非动态输出列名。例如：

```sql
SELECT key, value
FROM json_each('{"uid":1,"uname":"Alice"}');
```

```text
key    value
uid    1
uname  "Alice"
```

这条查询最终只输出 `key`、`value` 两列，因为 SELECT 显式选择了两列；换成 `SELECT *`，函数提供的八个普通列都可展开。`json`、`root`、`rowid` 通过单独虚拟列接口提供，不能把它们和八个普通输出列混为一谈。[固定普通列](/home/nijia.nj/nijia.nj/duckdb-main/extension/json/json_functions/json_table_in_out.cpp:9)、[虚拟列](/home/nijia.nj/nijia.nj/duckdb-main/extension/json/json_functions/json_table_in_out.cpp:366)、[官方 JSON 表函数文档](https://duckdb.org/docs/current/data/json/json_functions#json-table-functions)

如果 `users.json` 是 `[{"uid":1,"uname":"Alice"}]`，`read_json` 的 records 自动检测路径才会把 `uid`、`uname` 作为列名。因此“JSON 是否需要显式声明列”由具体函数的 schema 策略决定：固定结构遍历函数不需要，自动检测读取函数也可以不需要；它们做的工作不同。[检测后生成列名](/home/nijia.nj/nijia.nj/duckdb-main/extension/json/json_functions/read_json.cpp:213)、[绑定期间触发检测](/home/nijia.nj/nijia.nj/duckdb-main/extension/json/json_multi_file_info.cpp:302)、[官方 JSON 加载文档](https://duckdb.org/docs/current/data/json/loading_json)

## 不能把所有入口说成一条绑定路径

`SELECT unnest(...)` 在本地快照中是专用表达式绑定路径。列表展开的内部表达式名称可来自原表达式文本或别名；结构展开将成员名称带入多个输出表达式。这与 `FROM unnest(...)` 调用 `UnnestBind` 并固定输出 `unnest` 不同。[SELECT BindUnnest](/home/nijia.nj/nijia.nj/duckdb-main/src/planner/binder/expression/bind_unnest_expression.cpp:60)、[列表名称](/home/nijia.nj/nijia.nj/duckdb-main/src/planner/binder/expression/bind_unnest_expression.cpp:193)、[结构成员展开](/home/nijia.nj/nijia.nj/duckdb-main/src/planner/binder/expression/bind_unnest_expression.cpp:227)、[展开后的 SELECT 列名](/home/nijia.nj/nijia.nj/duckdb-main/src/planner/binder/query_node/bind_select_node.cpp:568)

本地 `FROM unnest` 明确只接收一个 LIST 参数，不能用当前在线文档中新增的多参数或结构展开能力替代这一事实。仓库已有测试包含单列表、别名及非法多参数情况；本次只读测试，没有执行。[输入检查](/home/nijia.nj/nijia.nj/duckdb-main/src/function/table/unnest.cpp:44)、[本地测试](/home/nijia.nj/nijia.nj/duckdb-main/test/sql/types/list/unnest_table_function.test:1)

通用表函数框架也提供 `bind_operator`、`bind_replace`，可以在常规 bind 前返回自定义算子或替换后的表引用；table macro 另外展开为查询。因此统一协议不等于每个入口都必定走相同执行算子。[替代绑定路径](/home/nijia.nj/nijia.nj/duckdb-main/src/planner/binder/tableref/bind_table_function.cpp:206)、[table macro 分支](/home/nijia.nj/nijia.nj/duckdb-main/src/planner/binder/tableref/bind_table_function.cpp:382)

## 对 seekdb 讨论的启示（设计推论）

DuckDB 证明了“统一表函数接口”和“保留语义明确的函数名”可以同时存在。可以让 `UNNEST`、JSON 遍历/读取、序列生成各自保留名字与参数规则，由各自的 bind 提交列名；查询层统一处理别名、星号展开和结果投影。是否额外提供 `TABLE(...)` 语法包装，是另一项选择。这是从上述源码接口归纳出的设计建议，不是对 seekdb 现有实现的事实描述。

如果 seekdb 希望把现有 PL 集合也接入这种协议，适配层可以在 bind 中读取集合元素定义并生成列名，在执行阶段遍历已有集合；生成器则在执行阶段按批次生成值。两者可以共享 schema/执行接口，而不要求相同的数据准备方式。JSON 的路径、固定列或自动检测策略仍由相应实现负责。

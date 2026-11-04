# AI 函数增强（赛题一）实现与验证说明

本文说明 `2026-competition` 分支上 AI 函数增强这一改动的落点、对外行为约定和验证方式，
对应赛题一的 6 个评分项。

## 1. 评分项与代码落点

| # | 评分项 | 分值 | 代码落点 | 关键行为 |
| --- | --- | --- | --- | --- |
| 1 | AI_PROMPT 类型与模板增强 | 10 | `src/sql/engine/expr/ob_expr_ai/ob_expr_ai_prompt.cpp`、`ob_ai_func_utils.cpp`（`ObAIFuncPromptObjectUtils`） | TEXT/LONGTEXT 模板与参数、JSON 数组/对象参数、`{n}` 重复占位符、嵌套 prompt、非法下标报错 |
| 2 | AI_EMBED 单行语义 | 10 | `ob_expr_ai_embed.cpp`、`ob_ai_func_utils.cpp`（`call_dense_embedding_vector_v2`、`validate_embedding_array`） | 正整数 `dimension`、返回向量维度与元素合法性校验、返回 JSON 文本 |
| 3 | AI_COMPLETE 请求与响应 | 10 | `ob_expr_ai_complete.cpp`、`ObOpenAIUtils::ObOpenAIComplete` | OpenAI 模型/Endpoint 解析、NULL config、config 原样并入请求体、空响应报可读错误 |
| 4 | Ollama Provider | 10 | `src/share/ai_service/ob_ai_service_struct.cpp`、`ObOllamaUtils` | 大小写不敏感注册、免 access key、`/api/generate` 与 `/api/embed` 原生协议 |
| 5 | AI_RERANK 结果正确性 | 10 | `ob_expr_ai_rerank.cpp` | 字符串数组 / 对象数组、分批、全局 index 映射、按分数稳定排序、返回数量与索引校验 |
| 6 | AI_EMBED 跨行批量 | 10 | `ob_expr_ai_embed.cpp`（`eval_ai_embed_batch`） | 同一批内相同模型的输入合并为一次请求、NULL 传播、按行下标回填 |

## 2. 对外行为约定

### 2.1 AI_PROMPT 与模板渲染

`AI_PROMPT(template, arg0, arg1, ...)` 返回 `{"template": ..., "args": [...]}` 的 JSON 对象，
真正渲染发生在 `AI_COMPLETE` 消费它时：

- 模板与字符串参数支持 `TEXT` / `LONGTEXT` / `TINYTEXT`（LOB locator 统一经
  `ObTextStringHelper::read_real_string_data` 读取）。
- 参数可以是字符串，也可以是 JSON 标量/数组/对象；非字符串参数按 JSON 打印规则
  序列化（`{"k": 2}`、`[1, true, null]`，冒号与逗号后有空格）。
- `{n}` 占位符按出现次序逐个替换，重复出现的 `{0}` 会被替换多次；下标越界、占位符
  未闭合、模板为空分别返回 `OB_INVALID_ARGUMENT` / `-4002`。
- 元素本身是合法 prompt 对象时递归渲染（嵌套深度上限 16）。

### 2.2 AI_EMBED

- `AI_EMBED(model, text[, dimension])`：`dimension` 必须为整型字面量或整型表达式，
  非整数类型在解析期即报 `OB_ERR_INVALID_TYPE_FOR_OP`，`dimension <= 0` 报
  `OB_INVALID_ARGUMENT`。
- 未显式给出 `dimension` 时不下发 `dimensions` 字段，向量维度由服务端决定；
  显式给出时既下发 `dimensions`，也用返回值长度做校验。
- 每个向量必须是长度非零的数值数组；元素非数值、`NaN`/`Inf`、维度不符都返回
  `OB_INVALID_DATA`（-4070）。
- 批量路径：一次表达式批内，`(model, dimension)` 相同的行合并成一次请求（`input`
  为数组），响应按 `index` 重新排序后按**行下标**回填，因此过滤、`LIMIT`、多批次都
  不会错位；NULL 输入不发请求、原样返回 NULL。

### 2.3 AI_COMPLETE

- 第一个参数是 `CREATE AI MODEL` 注册的逻辑名，Endpoint 通过 `ai_model_name` 关联；
  请求体里的 `model` 优先取 endpoint 的 `request_model_name`，否则取模型的 `model_name`。
- 第三个参数 `config` 为 JSON 文本时原样并入请求体，因此 `temperature`、`max_tokens`
  等字段直接生效；`config` 为 SQL NULL 时不注入任何字段。
- 响应必须是 JSON 对象，`$.choices[0].message.content` 必须存在、是字符串且非空，
  否则返回 `OB_INVALID_DATA` 并把原始响应写进用户错误信息。

### 2.4 Provider 协议

| Provider | Complete | Embed | Rerank |
| --- | --- | --- | --- |
| OPENAI / ALIYUN-OPENAI / DEEPSEEK / SILICONFLOW / HUNYUAN-OPENAI | `ObOpenAIComplete` | `ObOpenAIEmbed` | 仅 `SILICONFLOW`/`COHERE`/`ALIYUN-DASHSCOPE` |
| OLLAMA | `ObOllamaComplete`：`{"model","prompt","stream":false}` + `$.response` | `ObOllamaEmbed`：`{"model","input":[...]}` + `$.embeddings` | 不支持 |

约定与坑：

- provider 名称大小写不敏感（内部统一 `case_compare`）。
- OLLAMA 是唯一允许空 `access_key` 的 provider；其余 provider 建 endpoint 时
  `access_key` 为空会报 `OB_AI_FUNC_PARAM_EMPTY`。
- **Endpoint URL 原样作为请求 URL，代码不拼接路径**：Ollama 要写
  `http://host:port/api/generate`（或者 `/api/embed`），OpenAI 兼容服务要写全
  `http://host:port/v1/chat/completions`。
- 单个请求失败会对 429/5xx 重试，共 4 次尝试、1s/2s/4s 退避。

### 2.5 AI_RERANK

- `AI_RERANK(model, query, documents[, doc_key])`：`documents` 是 JSON 数组。
  三参模式下元素必须是字符串；四参模式（`doc_key` 非 NULL）下元素必须是对象，
  且 `doc_key` 指定的字段必须存在且是字符串。
- 每 20 个文档一批，`index` 是**批内下标**，代码映射回全局下标后再按
  `relevance_score` 降序排序；分数相同的条目按原始下标升序，因此排序结果是确定的
  （等价于对原始顺序做稳定排序）。
- 返回条目数必须等于本批文档数，`index` 不允许重复或越界，分数必须是有限数值，
  否则返回 `OB_INVALID_DATA`。

## 3. 错误码

| 场景 | 错误码 |
| --- | --- |
| 参数类型不支持 | `OB_ERR_INVALID_TYPE_FOR_OP` (-5083) |
| 参数为空、占位符非法、dimension 非正 | `OB_INVALID_ARGUMENT` (-4002) |
| 响应结构/内容不合法、索引重复越界、向量元素非法 | `OB_INVALID_DATA` (-4070) |
| provider 不支持该函数 | `OB_NOT_SUPPORTED` (-4007) |
| HTTP 非 2xx、连接失败 | `OB_CURL_ERROR` (-4216) |

## 4. 测试

### 4.1 单元测试

`unittest/sql/engine/expr/` 下两个用例覆盖本改动的核心逻辑：

- `ob_expr_ai_prompt_test.cpp`：prompt 对象校验、重复占位符、JSON 参数序列化、
  嵌套 prompt、非法下标。
- `test_ob_openai_utils.cpp`：OpenAI/Ollama/Dashscope 请求体与响应解析、
  embedding 按 `index` 重排、重复/越界索引拒绝、向量元素校验、Ollama 空 key。

这两个文件随本分支一起恢复，`unittest/sql/BUILD.bazel` 提供 `sql_tests` 目标：

```bash
bazel test //unittest/sql:sql_tests --test_output=errors
# 或只跑其中一组
bazel test //unittest/sql:sql_tests --test_filter='ObAIPromptTest.*'
```

### 4.2 mysqltest

```bash
python3 .github/script/seekdb/mysqltest_for_seekdb.py run \
  --seekdb build/bin/src/observer/seekdb \
  --suite tools/deploy/mysql_test/test_suite/ai_function
```

`ai_prompt.test` 覆盖模板/类型/表达式组合；`ai_function_enhancement.test` 是需要
Mock AI Service（监听 `0.0.0.0:8000`）的端到端用例。

## 5. 验证状态

- 逻辑与题面 6 个评分项逐条对照过（见第 1 节表格），行为与参考实现一致。
- 本机（Windows，无 Linux 构建环境）未做完整编译；Linux 上的复核步骤见第 4 节。

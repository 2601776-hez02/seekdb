# Mock AI Service（AI 函数自测用）

`mock_ai_service.py` 是一个只用标准库的假模型服务，用来在不连真实模型、不消耗额度的前提下
跑通 `AI_PROMPT` / `AI_COMPLETE` / `AI_EMBED` / `AI_RERANK` 与 Ollama 协议。
默认监听 `0.0.0.0:8000`，与赛题要求的 Mock AI Service 端口一致。

```bash
python3 tools/ai_mock/mock_ai_service.py            # 默认 0.0.0.0:8000，向量维度 4
python3 tools/ai_mock/mock_ai_service.py --port 8000 --dim 4
curl -s http://127.0.0.1:8000/__state              # 各类请求的调用计数
```

## 支持的路径

| 路径 | 用途 | 返回 |
| --- | --- | --- |
| `POST /v1/chat/completions` | OpenAI 兼容 chat completion | `$.choices[0].message.content = "mock: " + 最后一条 user 消息` |
| `POST /v1/embeddings` | OpenAI 兼容 embedding | `$.data[i].embedding`，维度取请求体的 `dimensions`，缺省 4 |
| `POST /api/generate` | Ollama 原生 completion | `$.response = "ollama: " + prompt`，`$.done = true` |
| `POST /api/embed` | Ollama 原生 embedding | `$.embeddings`，维度规则同上 |
| `POST /v1/rerank` | SiliconFlow / Cohere 风格 rerank | `$.results[i] = {index, relevance_score = 文档长度}` |
| `POST /api/v1/services/rerank/text-rerank/text-rerank` | DashScope rerank | 同上 |

## 建 endpoint 时的注意点

- OpenAI 兼容 provider 的 `access_key` 不能为空（Ollama 除外），随便给个假值即可。
- **URL 要写全路径**，代码不会自动补 `/v1/chat/completions`、`/api/generate` 之类的后缀。
- rerank 只支持 `SILICONFLOW` / `COHERE`（顶层 `results`）和 `ALIYUN-DASHSCOPE`
  （`output.results`），`OPENAI` 会返回 `OB_NOT_SUPPORTED`。

## 例子

AI 函数的第一个参数是 `CREATE_AI_MODEL` 注册的**模型名**；endpoint 通过
`ai_model_name` 与模型的 `model_name` 关联，所以两者要写成同一个名字。

```sql
call DBMS_AI_SERVICE.CREATE_AI_MODEL(
  'gpt_mock', '{"type":"completion","model_name":"gpt-4o-mini"}');
call DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(
  'mock_openai',
  '{"url":"http://127.0.0.1:8000/v1/chat/completions","provider":"openai",
    "access_key":"sk-mock","ai_model_name":"gpt-4o-mini"}');

call DBMS_AI_SERVICE.CREATE_AI_MODEL(
  'emb_mock', '{"type":"dense_embedding","model_name":"text-embedding-3-small"}');
call DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(
  'mock_openai_emb',
  '{"url":"http://127.0.0.1:8000/v1/embeddings","provider":"openai",
    "access_key":"sk-mock","ai_model_name":"text-embedding-3-small"}');

select ai_complete('gpt_mock', 'hello');
select ai_embed('emb_mock', 'hello', 4);
```

Ollama 走原生协议（没有 access_key）：

```sql
call DBMS_AI_SERVICE.CREATE_AI_MODEL(
  'ollama_gen', '{"type":"completion","model_name":"qwen2.5"}');
call DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(
  'mock_ollama',
  '{"url":"http://127.0.0.1:8000/api/generate","provider":"ollama",
    "ai_model_name":"qwen2.5"}');

select ai_complete('ollama_gen', 'hello');
```

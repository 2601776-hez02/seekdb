# AI gateway: usage metering and fault injection for AI COLUMN / AI functions

`gateway.py` is a transparent reverse proxy that sits between seekdb and a
model provider. It exists because two questions cannot be answered from inside
the database:

* **how much model traffic did a test or a workload really produce?**
  (how many calls, to which model, how many tokens, how much money)
* **what happens when the provider misbehaves?**
  (HTTP 429/500, slow answers, connections that die, and responses that are
  lost *after* the provider already did the work)

Standard library only, no credentials, no external network: it listens on
`127.0.0.1` and forwards to whatever `upstream` says.

## Where it sits

```
client SQL  ->  seekdb  ->  [ AI gateway :19121 ]  ->  provider or mock :19120
                                   |
                                   +-- GET /__usage   (JSON totals + per model)
```

The gateway is a **developer/test tool, not part of the database** and, most
importantly, **not part of the transaction boundary**:

1. `INSERT`/`UPDATE` commits the row and a task in one transaction;
2. *after* commit, the worker calls the model through the gateway;
3. the worker publishes the result in a second transaction, guarded by the
   source version fence.

Consequences that the counters reflect honestly:

* the gateway can see a call whose result the database then **discards**
  (the row was updated or deleted, or the column definition changed). The
  provider billed it, so the gateway counts it;
* the same row can produce **several calls** (retry after 5xx, lease expiry,
  crash recovery). All of them are counted;
* a **lost response** (`loss_times`) is counted as an upstream call *with*
  tokens, because the provider answered. The database sees a failure and
  retries, which produces the duplicate call that a naive "rows with a value"
  metric would hide.

So: `usage.totals.upstream_calls` is *money spent*, not *rows generated*.
To relate the two, compare `/__usage` before and after a statement, and read
`totals.retries` and `totals.lost_responses` to explain the difference.

## Running it

```bash
# proxy 19121 -> a local provider/mock on 19120
python3 tools/ai_gateway/gateway.py --upstream http://127.0.0.1:19120 --port 19121

# with a config file (faults, prices, ...)
python3 tools/ai_gateway/gateway.py --config tools/ai_gateway/gateway.json

# inspect a running gateway
python3 tools/ai_gateway/gateway.py --port 19121 --print-usage
python3 tools/ai_gateway/gateway.py --port 19121 --reset-usage
```

It prints `LISTENING <port> -> <upstream>` when ready, and a final
`USAGE {...}` line when it exits (Ctrl-C / SIGINT).

### CLI reference

| Option | Meaning |
| --- | --- |
| `--config FILE` | JSON config file (see below) |
| `--upstream URL` | upstream base URL, e.g. `http://127.0.0.1:19120` (required) |
| `--port N` | listen port, default 19121 |
| `--host H` | listen host, default `127.0.0.1` |
| `--delay-ms N` | fixed delay added to every request |
| `--fail-status CODE` | inject HTTP CODE (429, 500, 503, ...) |
| `--fail-times N` | how many requests to answer with `--fail-status` |
| `--reset-times N` | how many connections to reset without contacting upstream |
| `--loss-times N` | how many successful responses to swallow |
| `--print-usage` | client mode: print `GET /__usage` as JSON |
| `--reset-usage` | client mode: `POST /__usage/reset` |

### Config file reference

Every key is optional; CLI flags override the file.

| Key | Default | Meaning |
| --- | --- | --- |
| `upstream` | *required* | base URL of the provider or mock |
| `listen_host` / `listen_port` | `127.0.0.1` / `19121` | bind address |
| `timeout_ms` | `30000` | upstream timeout |
| `delay_ms` | `0` | fixed delay before forwarding |
| `delay_distribution` | `null` | `{"type":"uniform","min_ms":a,"max_ms":b}` extra delay |
| `fail_status` | `0` | `0` disables HTTP fault injection |
| `fail_times` / `fail_rate` / `fail_forever` | `0` / `0.0` / `false` | how often to inject `fail_status` |
| `reset_times` / `reset_rate` | `0` / `0.0` | connection reset without an upstream call |
| `loss_times` / `loss_rate` | `0` / `0.0` | swallow the response after a successful upstream call |
| `seed` | `0` | seed for `*_rate` and `delay_distribution` (reproducible faults) |
| `prices.currency` | `USD` | label only |
| `prices.per_1k_input` / `prices.per_1k_output` | `0.0` | flat price per 1000 tokens |
| `prices.models.<name>` | `{}` | per model override of the two prices above |

Fault order for one request: **delay -> `fail_status` -> reset -> upstream ->
loss**. A request answered by `fail_status` or reset never reaches the upstream
and therefore costs no tokens.

## Exact counting semantics

| Counter | Counted when | Billed? |
| --- | --- | --- |
| `totals.requests` | every request the gateway receives | no |
| `totals.upstream_calls` | the request was forwarded and the upstream answered (any status) | yes |
| `totals.injected_failures` | the gateway answered `fail_status` itself | no |
| `totals.injected_resets` | the gateway closed the connection before forwarding | no |
| `totals.upstream_errors` | forwarding raised (upstream down, timeout) | no |
| `totals.lost_responses` | upstream answered, response dropped on the floor | **yes** |
| `totals.retries` | a request whose `(model, input)` was already seen | depends |
| `totals.input_tokens` | `ceil(characters / 4)` of the prompt/input of an *upstream* call | yes |
| `totals.output_tokens` | `ceil(characters / 4)` of `choices[0].message.content` of an *upstream* call | yes |
| `totals.estimated_cost` | `sum(models: in/1000*p_in + out/1000*p_out)` | yes |

Notes:

* **Tokens are a heuristic**: four characters per token, rounded up, computed on
  the prompt text (`messages[].content` / `input`), not on the JSON envelope.
  It is stable and comparable across runs; it is not a tokenizer.
* **Retries are inferred, not observed.** The gateway does not retry anything.
  A "retry" is a second request with the same `(model, prompt)` digest. That is
  exactly what the database does after a 5xx, after a reset, and after a lost
  response - and it is also what two *different* rows with identical content
  look like. Use it as a hint, not as a proof.
* **Duplicate calls after response loss are counted twice**: once as the lost
  response (tokens billed) and once as the retry (tokens billed again). That is
  the truthful model of the provider's invoice, and it is the reason a lost
  response is more expensive than a plain 500.
* An injected `429`/`500` costs no tokens (the provider never saw the request),
  but it still counts in `totals.requests` and in `by_model.<m>.requests`,
  because the database did call the model.

`GET /__usage` returns:

```json
{
  "ok": true,
  "currency": "USD",
  "totals": {"requests": 4, "upstream_calls": 3, "injected_failures": 1,
             "injected_resets": 0, "upstream_errors": 0, "lost_responses": 1,
             "retries": 2, "input_tokens": 300, "output_tokens": 9,
             "total_tokens": 309, "estimated_cost": 0.000354},
  "latency_ms": {"avg": 41.3, "max": 87, "p50": 40, "p95": 87},
  "by_model": {"mock-complete": {"requests": 4, "upstream_calls": 3, "...": 0}},
  "config": {"upstream": "http://127.0.0.1:19120", "...": null}
}
```

## Wiring the database to the gateway

Create the logical model and point its endpoint at the gateway instead of the
provider. Nothing else changes - the gateway is transparent, including the
`Authorization` header.

```sql
-- logical model (same as usual)
call DBMS_AI_SERVICE.CREATE_AI_MODEL(
  'doc_model', '{
    "type": "completion",
    "model_name": "mock-complete"
 }');

-- endpoint -> the gateway, not the provider
call DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(
  'doc_endpoint', '{
    "ai_model_name": "doc_model",
    "url": "http://127.0.0.1:19121/v1/chat/completions",
    "access_key": "sk-mock",
    "request_model_name": "mock-complete",
    "provider": "openai"
  }');

-- an AI column that uses it
create table docs (
  id      bigint primary key,
  content varchar(8192),
  language varchar(32) AI COLUMN (
    AI_COMPLETE('doc_model', CONCAT('[[OUT=LANG]] Return only the language name. Content: ', content))
  ) WITH (sync_mode='async', max_retries=3)
);

-- measure one statement
--   shell: python3 gateway.py --port 19121 --reset-usage
insert into docs (id, content) values (1, '[[LANG=Japanese]] hello');
--   shell: python3 gateway.py --port 19121 --print-usage
```

After the insert, `totals.requests` is the number of times the worker called the
model for that row (one per AI column that fails or is retried), not the number
of rows.

With the mock service from the mysqltest suite the same wiring works, because
`test_suite/ai_function/include/ai_column_mock.py` speaks the same wire format:

```
gateway :19121  ->  ai_column_mock.py :19120
```

## Tests

```bash
python3 -m unittest discover -s tools/ai_gateway -p 'test_*.py' -v
python3 tools/ai_gateway/test_gateway.py -v
```

The tests start a fake upstream (in process, no credentials) and a gateway on
an ephemeral port. They cover pass-through, per model counters, the token
heuristic, injected 429/500, connection reset, response loss accounting,
delay, and cost estimation (flat and per model prices).

## Limitations

* HTTP(S) only as a plain proxy: no TLS termination, no HTTP/2, no streaming
  (`stream: true` responses are read whole before being forwarded).
* token counts are the documented 4-characters heuristic, not a tokenizer;
* retries are inferred from `(model, prompt)`, see above;
* one process, in-memory counters; restart the gateway to reset them (or call
  `POST /__usage/reset`).

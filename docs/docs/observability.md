# Observability & OpenTelemetry

Luotsi emits structured telemetry for every message that traverses the Core Bus. The observability layer is designed to produce **zero-overhead traces from the C++ runtime** without importing the full `opentelemetry-cpp` SDK. Instead, Luotsi emits telemetry as **CloudEvents 1.0 payloads** over UDP, which can be consumed directly by the built-in dashboard or forwarded to any compliant OTel Collector.

## Signal Architecture

The observability system emits two distinct CloudEvent types:

| Type | When emitted | Description |
|---|---|---|
| `luotsi.message` | Every `dispatch()` call | Point-in-time record of each routed frame (source, target, payload). |
| `luotsi.telemetry.span` | On request completion | Full **span** with start/end duration, W3C `traceparent`, and `gen_ai.*` semantic attributes. |

Spans are only emitted when a request/response cycle completes through the `PendingRequests` stateful tracker — i.e., when an agent calls `tools/call` and the downstream MCP server returns a result.

## Transport: UDP CloudEvents

All events are emitted as newline-delimited JSON over a **fire-and-forget UDP socket** to the configured `observability_endpoint` (default `127.0.0.1:9000`). This approach was chosen deliberately:

*   **Non-blocking**: UDP emission never stalls the core `asio` event loop. A dropped packet causes no side-effects.
*   **Protocol-agnostic**: Any UDP listener (Node.js dashboard, Fluentd, a local OTel Collector) can consume the stream without coupling to C++ internals.
*   **Structured**: Full CloudEvents 1.0 envelope ensures compatibility with the CNCF ecosystem.

## W3C Trace Context Propagation

Luotsi implements [W3C Trace Context Level 1](https://www.w3.org/TR/trace-context/) via the `traceparent` header embedded in a JSON-RPC `_meta` block.

### Ingress (Extraction at the Adapter Edge)

When a frame arrives at either `StdioAdapter` or `JsonRpcTcpAdapter`, the adapter attempts to extract a `traceparent` from two candidate locations in order:

1. `payload["_meta"]["traceparent"]` — top-level meta (non-MCP envelopes)
2. `payload["params"]["_meta"]["traceparent"]` — MCP-standard params meta block

If a valid `traceparent` is found (format: `00-<32-hex-trace-id>-<16-hex-parent-span-id>-<flags>`), the `trace_id` and `parent_span_id` are extracted and stored on the `MessageFrame`. A fresh `span_id` is generated for the new hop.

If no `traceparent` is found, **Luotsi generates a new root `trace_id`**. This allows trace fragmentation rather than blocking agents that do not propagate context — a deliberate design choice for maximum compatibility with uninstrumented agents.

### Egress (Injection into Downstream Calls)

When `Runtime::dispatch()` forwards a frame to a downstream MCP server, it injects the active `traceparent` back into the outbound payload:

*   If the payload has a `params` object: injected as `params._meta.traceparent`
*   Otherwise: injected as `_meta.traceparent`

This means any MCP server instrumented with a standard OTel SDK will automatically inherit the correct parent span context without needing to know about Luotsi.

### Trace Context on the `MessageFrame`

The `luotsi::MessageFrame` struct carries trace context across the internal bus:

```cpp
struct MessageFrame {
    std::string source_id;
    std::string target_id;
    std::string delegated_role;
    nlohmann::json payload;
    // W3C Trace Context
    std::string trace_id;       // 32-hex lowercase, no hyphens
    std::string span_id;        // 16-hex lowercase, no hyphens
    std::string parent_span_id; // populated if agent supplied traceparent
    std::chrono::steady_clock::time_point timestamp; // span start time
};
```

## Stateful Span Generation

The `Runtime` maintains a `PendingRequestState` map (`pending_requests_`) that tracks the open span context for every in-flight request:

```cpp
struct PendingRequestState {
    std::string source_id;
    std::string trace_id;
    std::string span_id;
    std::string parent_span_id;
    std::chrono::steady_clock::time_point start_time;
};
```

**Span lifecycle:**

1.  **Span Open** — When an agent sends a `tools/call` (or any routed request with an `id`), the Core records a `PendingRequestState` keyed on the NAT'd global `Request ID`.
2.  **Span Close** — When the matching JSON-RPC response arrives from the MCP server, the Core resolves the `duration_ms` and calls `Observability::log_span()`.
3.  **Emit** — A `luotsi.telemetry.span` CloudEvent is emitted over UDP and written to the audit log.

## CloudEvent Schema

### `luotsi.message` (per-dispatch)

```json
{
  "specversion": "1.0",
  "type": "luotsi.message",
  "source": "luotsi-core",
  "id": "<uuid-v4>",
  "time": "2026-04-17T19:37:23Z",
  "datacontenttype": "application/json",
  "luotsisource": "langchain_agent",
  "luotsitarget": "odoo_mcp",
  "data": {
    "delegated_role": "",
    "payload": { "jsonrpc": "2.0", "method": "tools/call", "..." }
  }
}
```

### `luotsi.telemetry.span` (per-completed request)

```json
{
  "specversion": "1.0",
  "type": "luotsi.telemetry.span",
  "source": "luotsi-core",
  "id": "<uuid-v4>",
  "time": "2026-04-17T19:37:23Z",
  "traceparent": "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01",
  "datacontenttype": "application/json",
  "data": {
    "name": "luotsi.route",
    "kind": "SERVER",
    "duration_ms": 142,
    "status": "OK",
    "attributes": {
      "gen_ai.system": "luotsi_switch_fabric",
      "gen_ai.agent.name": "langchain_agent",
      "rpc.system": "jsonrpc",
      "rpc.service": "odoo_mcp"
    }
  }
}
```

### GenAI Semantic Convention Mapping

Luotsi follows the [OpenTelemetry GenAI Agent Spans](https://opentelemetry.io/docs/specs/semconv/gen-ai/gen-ai-agent-spans/) semantic conventions (status: Development):

| OTel Attribute | Luotsi Source | Notes |
|---|---|---|
| `gen_ai.system` | Static: `"luotsi_switch_fabric"` | Identifies Luotsi as the instrumentation provider |
| `gen_ai.agent.name` | `PendingRequestState::source_id` | The authenticated agent node that originated the request |
| `rpc.system` | Static: `"jsonrpc"` | Transport-level protocol |
| `rpc.service` | `MessageFrame::source_id` (on response) | The MCP server node that fulfilled the request |
| `rpc.jsonrpc.error_code` | `response.payload["error"]["code"]` | Only set when `status == "ERROR"` |

## Configuration

Enable observability in your `config.yaml`:

```yaml
observability:
  endpoint: "127.0.0.1:9000"   # UDP target for CloudEvents
  log_file: "audit.jsonl"       # Optional: JSONL file audit log
```

Both outputs are independent — you can use only UDP (for the live dashboard), only the log file (for audit compliance), or both simultaneously.

## Integration with the Dashboard

The Luotsi Dashboard (`dashboard/`) natively consumes the UDP stream on port `9000`. It differentiates between event types:

*   `luotsi.heartbeat` — topology state (active nodes, capabilities)
*   `luotsi.message` — displayed in the Events Terminal with source→target routing visualization
*   `luotsi.telemetry.span` — **new**: span events can be consumed for latency tracking (future: Gantt visualization per trace)

## Forwarding to an OTel Collector

Since Luotsi emits canonical CloudEvents, you can forward them to any OpenTelemetry Collector using the [CloudEvents receiver](https://github.com/open-telemetry/opentelemetry-collector-contrib/tree/main/receiver/cloudeventsreceiver). A minimal sidecar in Node.js can also convert the UDP stream to OTLP gRPC:

```
[Luotsi Core] --UDP--> [Node.js Collector Sidecar] --OTLP gRPC--> [Jaeger / Datadog / Honeycomb]
```

The `traceparent` field is preserved in the CloudEvent envelope, enabling downstream collectors to correctly reconstruct the distributed trace tree across Luotsi, LangChain agents, and MCP servers with no modification to any component.

## Audit Log Format

When `log_file` is configured, Luotsi appends each CloudEvent as a single-line JSON record to the file (JSONL format). This produces a tamper-evident, ordered audit trail suitable for compliance and forensic analysis:

```
{"specversion":"1.0","type":"luotsi.message",...}
{"specversion":"1.0","type":"luotsi.telemetry.span",...}
```

The file is opened in **append mode** and writes are mutex-protected, making it safe for inspection while Luotsi is running.

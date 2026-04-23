# Distributed Tracing Architecture

This document details how Luotsi Core handles distributed trace context propagation, causal inference, and trace memory lifecycles across uninstrumented multi-agent boundaries.

## 1. Trace Context Extraction and Inference

Luotsi natively implements W3C Trace Context (Level 1). Trace IDs (`traceparent`) are typically extracted from JSON-RPC `_meta` blocks at the adapter ingress edge (`StdioAdapter`, `JsonRpcTcpAdapter`). 
However, because many external agents and standard MCP servers are uninstrumented and do not natively propagate trace contexts, Luotsi utilizes an internal routing engine to synthesize missing tracing contexts transparently.

### 1.1 Ingress without Trace Context
When the adapters receive a payload lacking a `traceparent`, they leave the `MessageFrame` trace context explicitly empty. They do not preemptively generate randomized trace IDs. By preserving the empty state, the core routing logic (`Runtime::route_message()`) is allowed to intercept the frame and attempt contextual reconstruction.

### 1.2 Causal Inference (`find_most_likely_parent`)
If an intermediate node (e.g., `langchain_agent`) dispatches a sub-request (like a `tools/call` to `odoo_mcp`) without supplying its own W3C trace header, the Core infers the causal parent. 
The routing engine scans the `nat_insertion_order_` queue backward, querying the `nat_table_` for the most recent **unfulfilled** inbound request where `target_id` matches the originating node.

If a matching unfulfilled inbound request is found, Luotsi transparently inherits the active `trace_id` and assigns the parent's `span_id` as the `parent_span_id` for the new sub-request.
If no parent is found (indicating a true root caller, such as an integration gateway sending a new message), the Core generates a new root `trace_id`.

## 2. Spans vs. Traces

Luotsi explicitly differentiates between individual node hops ("spans") and complete end-to-end multi-agent session lifecycles ("traces").

*   **Span**: An atomic request/response cycle between two nodes over the Luotsi bus. The completion of an internal span emits a `luotsi.telemetry.span` CloudEvent containing latency metrics and semantic properties mapping to the OpenTelemetry GenAI agent conventions.
*   **Trace**: The entire graph of causal spans radiating from a primary gateway ingress request. 

### Gateway Nodes (`is_gateway`)
Nodes defined in the configuration with `is_gateway: true` dictate the absolute input/output boundaries of a trace lifecycle. A trace officially resolves when the final top-level response routes back to an `is_gateway` node.
At this boundary, the Core injects the custom attribute `luotsi.trace_end = true` into the final span event. This explicit marker signals to external observability platforms and the internal Luotsi dashboard to finalize the session grouping.

## 3. Stateful NAT Table Memory Lifecycle

Luotsi maintains a Network Address Translation (`nat_table_`) state map to correlate asynchronous JSON-RPC responses and support the causal inference engine. Because high-throughput multi-agent messaging and discovery polling can consume significant memory, trace lifecycle garbage collection is deterministic.

*   **Immediate Eviction**: When a node response successfully hits the Luotsi Core and fulfills an active NAT entry, the entry is **immediately** erased from `nat_table_` and the chronological `nat_insertion_order_` deque. There is no passive GC limit threshold.
*   **Trace Context Termination**: When a response successfully hits a node marked with `is_gateway: true`, the routing engine assumes the end of the user transaction and explicitly drops any cached session correlation state (`session_trace_cache_`). This ensures that consecutive requests from the same user across a gateway are distinctly partitioned into independent root trace IDs, preventing unmanageable mega-trace stitching.

This deterministic memory lifecycle ensures `O(1)` cleanup latency while structurally guaranteeing OOM prevention even under intensive asynchronous loads.

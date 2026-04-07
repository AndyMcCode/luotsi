# Luotsi Governance: Guardrails & Enforcement

Luotsi implements a dual-layer security model to control agent access to MCP tools and resources. This ensures that even if an agent is compromised or over-eager, it cannot perform unauthorized actions.

## 1. Discovery-Level Filtering ("Soft Guardrail")
The first layer of defense happens during the MCP initialization phase. When an agent requests a list of tools via `tools/list`, Luotsi Core intercepts the response from the MCP servers and filters it based on the agent's role.

- **How it works**: Luotsi removes unauthorized tool definitions from the JSON-RPC response before it ever reaches the agent.
- **Goal**: The agent's LLM remains unaware that certain capabilities even exist. This prevents the LLM from attempting to hallucinate or probe restricted functions.
- **Configuration**: Defined via `allowed_tools` and `blocked_tools` in `policies.yaml`.

## 2. Bus-Level Enforcement ("Hard Guardrail")
The second, immutable layer of defense happens at the message bus level. Every time an agent attempts to execute a tool via `tools/call`, Luotsi Core again verifies the request against the policy engine.

- **How it works**: Even if an agent bypasses the discovery layer (e.g., by hardcoding a tool name it should not know), the Core will intercept the `tools/call` message and reject it immediately if the policy does not permit it.
- **Goal**: Absolute security. No message can cross the bus to an MCP server without explicit authorization.
- **Error Response**: Unauthorized calls return a standard JSON-RPC error with code `-32001` (Access Denied).

## Hierarchical Precedence
Luotsi follows a **"Deny-Wins"** strategy combined with **Least Privilege**:
1. If a tool matches a `blocked_tools` pattern, it is rejected (Hard Block).
2. If it does not match an `allowed_tools` pattern (when explicit allows are used), it is rejected.
3. Access is only granted if it passes both the discovery filter and the bus-level check.

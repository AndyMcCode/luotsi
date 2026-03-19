# Luotsi Routing Reference

Luotsi uses a YAML-defined routing system to direct JSON-RPC messages between nodes on its internal bus.

## Master Node (Catch-all)
A node can be designated as the **master** node using the `master: true` flag in its configuration.
- **Behavior**: If no other route matches a message, it is forwarded to the master node.
- **Anti-Loop**: Luotsi will not forward a message back to the master node if it originated there.
- **Priority**: Fallback routing occurs only after all explicit routes and policy checks are exhausted.

Example:
```yaml
nodes:
  - id: "planner"
    master: true
    ...
```

## Routing Keywords

| Keyword | Description |
| :--- | :--- |
| `trigger` | Substring match against the `method` (e.g., `tools/call`, `initialize`). |
| `namespace` | Prefix match against the `method` before a colon (e.g., `odoo:`). |
| `target` | The node `id` to which the message should be forwarded. |
| `action` | Special Luotsi Core action (e.g., `translate`, `mcp_call_router`, `fan_out_mcp`). |
| `new_method` | (Optional) Renames the method when forwarding (used with `translate`). |

## Hierarchy and Precedence
Luotsi evaluates routes in the following order for each message:
1. **Explicit Routes**: Defined in the source node's `routes` list.
   - `namespace` matches have higher priority than `trigger` matches within a single node.
2. **Special Actions**: If a `tools/call` or `resources/list` trigger matches an action like `mcp_call_router`, Luotsi uses internal metadata to route the call.
3. **Master Fallback**: If no explicit route remains after policy filtering, the message goes to the `master` node.

## Minimal Routing Template
To get an agent working with MCP servers:
```yaml
nodes:
  - id: "agent"
    routes:
      - trigger: "initialize"
        action: "mcp_registry_query"
        targets: ["mcp_server_1"]
      - trigger: "tools/call"
        action: "mcp_call_router"
```

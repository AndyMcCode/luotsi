# Policies and Governance

Luotsi includes a built-in Policy Engine to enforce Role-Based Access Control (RBAC) and govern interactions between agents and MCP servers.

## Role Definition

Policies are defined in a dedicated YAML file (e.g., `policies.yaml`), which is referenced in the global configuration (`policies_file: "policies.yaml"`).

A policy file defines roles, secret keys, and the specific servers each role is allowed to access.

```yaml
roles:
  - name: "admin_agent"
    secret_key: "super_secret_admin_key"
    allowed_servers:
      - "*" # Access to everything
      
  - name: "limited_agent"
    secret_key: "limited_key_123"
    allowed_servers:
      - "weather_mcp"
      - "calculator_mcp"
```

## Agent Authentication

Before an agent can query the registry or interact with bounded servers, it must be assigned a role. Luotsi provides two ways to authenticate an agent:

### 1. Static Configuration Assignment
The simplest method is assigning a `role` directly to the node inside `multi_agent.config.yaml`. The agent adopts this identity implicitly on startup.
```yaml
nodes:
  - id: "my_agent"
    is_agent: true
    role: "admin_agent"
```

### 2. Runtime Handshake
Alternatively, for dynamic endpoints (like a TCP socket), an agent can manually authenticate by sending a `luotsi/authenticate` custom JSON-RPC method to the runtime containing a `secret_key` matching `policies.yaml`.

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "luotsi/authenticate",
  "params": {
    "secret_key": "limited_key_123"
  }
}
```

If successful, the runtime associates the connection's source ID with the corresponding role (`limited_agent`).

## Policy Enforcement

Policy enforcement occurs natively at the routing layer.

### Registry Filtering (`mcp_registry_query`)
When an authenticated agent requests `tools/list` via the central registry, the Policy Engine intervenes. It checks the agent's assigned role and its `allowed_servers` list.

The response from the internal cache is filtered so the agent *only* sees tools, resources, and prompts from the MCP servers it is explicitly permitted to access. If an agent with the `limited_agent` role asks for tools, it will not see any tools belonging to the `database_mcp` server.

### Capability Blocking (`disabled_capabilities`)
Administrators can also proactively disable specific capabilities on a per-node basis using `disabled_capabilities` in `node.yaml`.

```yaml
nodes:
  - id: "legacy_system"
    is_mcp_server: true
    disabled_capabilities:
      - "tools/list"
```

This prevents Luotsi from querying those capabilities during auto-discovery, effectively hiding them from the entire runtime environment regardless of role.
### Tool-Level Governance

Luotsi supports granular control over which tools and resources an agent can access within an allowed MCP server. This follows a **Deny-Wins** principle: if a tool matches a pattern in `blocked_tools`, it is denied even if it matches a pattern in `allowed_tools`.

#### Configuration Syntax

Restrictions are defined in `policies.yaml` under each role:

```yaml
roles:
  - name: "guest"
    allowed_servers: ["odoo_mcp"]
    allowed_tools:
      - "odoo_mcp:search_*"      # Wildcard support
      - "weather_mcp:get_forecast"
    blocked_tools:
      - "odoo_mcp:execute_kw"    # Explicit deny
    allowed_resources:
      - "odoo://models"
```

#### Wildcard Matching
- `*`: Matches everything.
- `prefix:*`: Matches any tool/resource starting with the prefix.
- `exact_name`: Matches only that specific tool/resource.

#### Policy Enforcement Layers

Luotsi implements a two-layered defense strategy to handle both agent behavior (visibility) and system safety (enforcement).

1. **Soft Guardrails (Discovery Filtering)**:
   - **Mechanism**: Luotsi intercepts `tools/list`, `resources/list`, and `resources/templates/list` calls at the registry layer.
   - **Goal**: Manage LLM visibility. By removing unauthorized tools before they reach the agent, we prevent the "over-informed agent" problem. If an agent doesn't see a tool in its configuration, it is unlikely to attempt a call, leading to a smoother user experience without unnecessary errors.
   - **Timing**: Occurs during agent initialization or periodic discovery.

2. **Hard Guardrails (Execution Blocking)**:
   - **Mechanism**: Luotsi validates every `tools/call` and `resources/read` message passing through the bus.
   - **Goal**: Absolute safety and multi-tenancy. Even if an agent "remembers" a tool from a previous session or a different user context (in shared-agent scenarios), the Hub will intercept and reject the execution if the **active delegated role** does not have permission.
   - **Timing**: Occurs at runtime, milliseconds before a request would reach an MCP server. Unauthorized calls are rejected with error code `-32001`.

### Outbound Method Governance (`allowed_methods`)

To restrict the fundamental protocol capabilities of a node, you can define exactly which JSON-RPC outbound methods a role is allowed to emit using `allowed_methods`.

This is incredibly powerful for locking down "dumb" nodes (like external MCP servers) to ensure they only broadcast status updates and cannot mistakenly or maliciously invoke active procedures.

```yaml
roles:
  - name: "sandbox_mcp_server"
    allowed_methods:
      - "notifications/*"
      - "roots/list"
    # An MCP server should never emit 'tools/call'. By omitting it here, it is strictly denied.
```
*Note: If `allowed_methods` is entirely omitted from a role definition, Luotsi defaults to allowing all methods to preserve backward compatibility with legacy Agents.*

## Stateful Session Integrity

In legacy iterations, Luotsi relied on payload injection (such as attaching a `__luotsi_role__` metadata field into the JSON) to track multi-tenant identities through the message bus and downstream MCP servers.

Under the Zero-Trust IAM architecture, Luotsi **strictly forbids** payload manipulation to track identities, ensuring 100% compliance with the native JSON-RPC 2.0 specification without "dirty" root payload injections.

### Stateful Request/Response Routing

Luotsi eliminates the need for downstream MCP servers to mirror identities or roles back to the core. Instead, it enforces isolated session integrity through stateful memory mapping.

1.  **Outbound Context Binding**: When an authenticated Agent routes a request (e.g., `tools/call`) to an MCP server, the Luotsi Core intercepts the message and securely records the interaction in an internal, thread-safe `PendingRequests` table. This maps the `Request ID` directly to the `Client Node ID` and `Client Role` in memory.
2.  **Inbound Enforcement**: When the MCP server processes the target function and emits a JSON-RPC response (identified by the matching `id` with no `method`), Luotsi catches it at the Port.
3.  **Direct Routing**: The Core cross-references the matching `id`, retrieves the exact authenticated `Client Node ID` from memory, cleanly stamps the outbound `MessageFrame` target, and fires the payload directly back to the requester.

### Security Outcomes

Because roles are mapped organically within the Adapter's isolated memory and state is maintained within the Core's routing table:
*   Downstream nodes are completely blind to the routing mechanism and do not need to support custom metadata fields.
*   It is physically impossible for a compromised or lateral node to artificially inject or spoof roles to escalate privileges.
*   East-West lateral movement between unauthorized elements is definitively blocked by the Native Policy Engine.

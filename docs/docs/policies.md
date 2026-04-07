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

## Role Delegation (On-Behalf-Of)

Role Delegation allows a trusted node (e.g., a multi-tenant gateway or agent) to act on behalf of another role. This is essential for multi-user scenarios where a single agent instance handles requests for users with varying permission levels.

### Trusted Sources (`is_trusted`)

Only roles marked as `is_trusted: true` in `policies.yaml` are permitted to perform role delegation. 

```yaml
roles:
  - name: "whatsapp_gateway"
    secret_key: "gateway_secret_123"
    is_trusted: true
    allowed_servers: ["*"]
```

### Delegation Metadata (`__luotsi_role__`)

To delegate, a trusted source includes the `__luotsi_role__` metadata in its JSON message payload.

1.  **Ingress**: A gateway (e.g., WhatsApp Mock) identifies an end-user and injects `"__luotsi_role__": "guest"` into the message.
2.  **Detection**: The Luotsi Core extracts this value into the internal `MessageFrame::delegated_role` field.
3.  **Governance**: If the source node is `is_trusted`, the Core switches the *active role* to the delegated role for all policy checks (token limits, server access).
4.  **Propagation**: The Core re-injects `__luotsi_role__` when sending the message to the target (e.g., the Agent), ensuring the context is preserved across multi-hop execution.

### Hierarchical Quotas

When delegation occurs, Luotsi enforces the strictest combined policy. For example, if an Admin gateway is acting as a Guest, the Guest's lower `max_token_size` and restricted `allowed_servers` will be enforced for that specific interaction.

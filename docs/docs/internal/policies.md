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

Before an agent can query the registry or interact with bounded servers, it must authenticate with the Luotsi Core Runtime.

The agent sends a `luotsi/authenticate` custom JSON-RPC method:

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

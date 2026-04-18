# Multi-Tenancy & Zero-Trust RBAC

Luotsi enforces a strictly compartmentalized **Zero-Trust Role-Based Access Control (RBAC)** architecture natively. Permissions are bound unequivocally across the transport lifecycle using physical memory isolation rather than vulnerable payload injection tricks.

---

## The Core Concept

Luotsi operates a central **Native Policy Engine** that acts as the physical gatekeeper inside the switch fabric (`Runtime::route_message`).

Instead of trusting arbitrary incoming JSON-RPC methods, the routing engine identifies the physical C++ adapter port that delivered the message. It invokes a polymorphic call back to the Edge Adapter to definitively establish its memory-resident identity, and then rigorously assesses the intent against the `policies.yaml` definitions before allowing data to traverse downstream.

---

## Edge Authentication (The Adapters)

Luotsi does not permit authentication parameters to bleed past the edge. All identities are verified and permanently assigned right at the adapter boundary instance.

### 1. Stdio Adapters (Local Spawn)
When Luotsi `fork()`s a subprocess internally (like an internal Dockerized Agent or MCP Service), the system grants inherent trust. 

During initialization, the Stdio Adapter reads its designated identity string definitively from the `multi_agent.config.yaml` `role:` property. It automatically boots into an `ESTABLISHED` capability state mapped seamlessly to the core port.

*Note: If an internal node has `is_mcp_server: true` but omits a specific role, Luotsi automatically maps it to a safe `mcp_server` sandbox role to isolate capability lookups.*

### 2. TCP Adapters (Stateful Unlocks)
When external Agents connect over an asynchronous socket (e.g., `JsonRpcTcpAdapter`), they are inherently untrusted. The adapter drops into a rigid `AUTHENTICATING` firewall state.

**The Handshake Protocol:**
The agent must submit a standard JSON-RPC `initialize` block, embedding a `_meta` field holding its pre-shared secret key. 

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "initialize",
  "params": {
    "protocolVersion": "2024-11-05",
    "clientInfo": {"name": "external_agent"},
    "_meta": {
      "luotsi_auth": "sk-remote-agent-abc"
    }
  }
}
```

- If validation passes, the Adapter rips the `_meta` block off, caches the localized role in its class instance, and pushes the pure `initialize` frame backward into the central router. The connection formally advances to the `ESTABLISHED` state.
- If an agent sends *any* other method (like `tools/call`) prior to successful initialization, or misses the correct key, the adapter throws an error and instantly snuffs the TCP connection dead.

> [!IMPORTANT]  
> After initialization is cleared, **do NOT** send `_meta` blocks. Luotsi is 100% compliant with generic MCP JSON-RPC 2.0 downstream processing natively because the TCP edge now statefully manages identity internally.

---

## Central Routing & Enforcement

Once a frame enters `Runtime::route_message`, Luotsi's core validates its intent implicitly:

```cpp
// 1. Trace the Port
auto agent_port = std::dynamic_pointer_cast<ports::AgentPort>(ports_[source_id]);
std::string base_role = agent_port->getRole(); // The active statefully verified role

// 2. Validate Methods
if (!is_authorized(base_role, method)) {
    // Generates a proper native JSON-RPC (-32001) Access Denied payload dynamically back to the client!
    return;
}
```
If an unprivileged remote agent attempts an explicit system call or out-of-scope tool invocation, it is stopped instantly at the gate.

---

## Policy Definitions

Identities and restrictions are statically loaded from `configs/policies.yaml`.

```yaml
roles:
  - name: "admin"
    secret_key: "sk-local-admin-999"
    allowed_servers: ["*"]
    allowed_methods: ["*"]
    
  - name: "agent"
    secret_key: "sk-remote-agent-abc"
    allowed_servers: ["odoo_mcp", "memory_mcp"]
    allowed_methods: ["tools/call", "resources/read"]
    blocked_tools:
      - "odoo_mcp:execute_kw"     # Finer-grained internal resource bounding
```

### Constraints Supported
| Field | Target | Description |
|---|---|---|
| `allowed_methods` | Global API Gatekeeper | Constrains the overarching JSON-RPC method actions permitted (e.g. `tools/call`, `resources/read`). |
| `allowed_servers` | Routing Level | Dictates exactly which downstream MCP Ports the node is allowed to transmit toward. |
| `allowed_tools` | Method Level | Binds execution specifically down to prefix or full method chains (e.g. `memory_mcp:get_fact`). |

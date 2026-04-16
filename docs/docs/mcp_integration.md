# MCP Integration

Luotsi provides native, first-class support for the Model Context Protocol (MCP). Rather than just passing MCP messages opaquely, the Luotsi Core Runtime actively participates in the protocol via the **McpPort** to provide auto-discovery, capability caching, and intelligent routing.

## Auto-Discovery and Handshake

When a node is configured as an MCP server (`is_mcp_server: true`), Luotsi automatically initiates an auto-discovery handshake upon startup. This is handled by a specialized `McpPort` instance that communicates via a physical `IAdapter`.

```yaml
nodes:
  - id: "my_mcp_service"
    is_mcp_server: true
    runtime:
      adapter: stdio
      # ... command/args ...
```

The runtime will:
1. Send an `initialize` request to the server on behalf of the `luotsi-hub`.
2. Wait for the `notifications/initialized` response.
3. Automatically query the server for its available capabilities, including:
   - `tools/list`
   - `resources/list`
   - `resources/templates/list`
   - `prompts/list`

## Coordinated Startup (Dependency Orchestration)

To prevent race conditions during startup (e.g., an agent starting before its required MCP tools are discovered), Luotsi Core supports a `depends` field in the node configuration.

```yaml
nodes:
  - id: "my_mcp_service"
    is_mcp_server: true
    # ... runtime config ...

  - id: "my_agent"
    depends: ["my_mcp_service"]
    # ... runtime config ...
```

When `depends` is used:
1. The dependent node (e.g., `my_agent`) is held in a **deferred** state.
2. Luotsi Core spawns the dependency nodes first.
3. If a dependency is an MCP server, Luotsi waits for the **entire auto-discovery handshake** to complete (tools, resources, templates, and prompts).
4. Only once all dependencies are fully initialized and cached does Luotsi Core spawn the dependent node.

## Resource Interaction

Beyond tools, Luotsi Core discovers and caches MCP **Resources** and **Resource Templates**.

- **URIs**: Discovery queries populate the internal registry with URIs (e.g., `odoo://model/res.partner`).
- **Resource Read**: Agents can access these resources by emitting a `resources/read` request with the appropriate URI.
- **Dynamic Context**: Agents can use templates to construct specific URIs (e.g., `odoo://record/res.partner/1`) for direct data access without using tools.

## Capability Caching

To reduce latency and overhead, the Core Runtime caches the results of these discovery queries in an internal `McpRegistry`. 

When an agent node (configured with `is_agent: true`) sends a capabilities query (e.g., `tools/list`) to the runtime, it is automatically routed using the intrinsic `mcp_registry_query` action. Luotsi serves the response directly from its internal cache rather than forwarding the request to the upstream MCP servers. This is part of the **ACP (Agent Core Protocol)** implementation, ensuring agents see a unified, namespaced view of all tools restricted strictly to their assigned policy role.

## Prefixing and Namespacing

For example, if an MCP server with ID `odoo_mcp` exposes a tool named `search_customers`, Luotsi will cache and expose it to clients as `odoo_mcp__search_customers`.

## Native Protocol Engine (Lifecycle Governance)

Luotsi Core features a **Native Protocol Engine** that automatically intercepts and manages standard MCP lifecycle protocols. This drastically simplifies your node configurations, eliminating the need to write complex routing logic for standard protocol operations.

### Sandbox Boundaries (`roots/list`)

When an MCP server requires filesystem boundaries, it sends a `roots/list` request. Instead of routing this request to an external node, Luotsi Core's Native Engine intercepts it. The core natively generates the strict JSON boundary response by extracting paths from your node's `allowed_roots` configuration.

```yaml
nodes:
  - id: secure_git_server
    is_mcp_server: true
    allowed_roots: ["/home/user/project_repo"]
    runtime:
      adapter: stdio
      command: npx
      args: ["-y", "@modelcontextprotocol/server-git"]
```

### Dynamic Capability Sync (`notifications/tools/list_changed`)

If an MCP server's active tools change dynamically during its execution lifecycle, it raises a `notifications/tools/list_changed` JSON-RPC message. The Native Engine flawlessly intercepts this notification and instantly halts the transaction gracefully. Following interception, it constructs a new internal capabilities fetch request (e.g., `__luotsi__tools__<node_id>`) and shoots it directly back at the server.

Upon receiving the subsequent tools response, Luotsi dynamically overwrites the `McpRegistry` cache, ensuring that agents always possess an actively synchronized representation of network tools. Similar operations natively occur for `resources/list_changed` and `prompts/list_changed`.

### Overriding Native Hooks

While Native Protocol Hooks drastically simplify operations out-of-the-box, advanced architects can still override the Luotsi Core's default behaviors locally. 

If you configure an **explicit, exact route match** in a node's `routes` list (e.g., `trigger: "roots/list"` or `trigger: "notifications/tools/list_changed"` mapping to a custom network target), Luotsi disables the Native Engine intercept for that exact request, steps down, and routes the transaction securely to your custom target. 

*(Note: Standard catch-all wildcard routes (e.g., `trigger: "*"`) are purposely ignored by the override evaluator. This guarantees that your fallback routing rules don't accidentally cripple the Native Engine's core functionality!)*

### Protocol Noise Suppression (Route-or-Absorb)

Protocol anomalies and informational events like `notifications/cancelled` or `notifications/progress` are safely evaluated by the Native Engine using a "Route-or-Absorb" mechanism.

When an MCP server emits a standard notification:
1. **Explicit Routing**: Luotsi checks if you have actively configured a route for that specific notification. If you have, Luotsi stands down and routes the payload natively across the bus.
2. **Native Absorption**: If no route is defined, Luotsi determines the notification is untracked telemetry "noise". Instead of flooding your logs with "No Route Found" errors, it gracefully absorbs the packet and drops it.

#### Example: Capturing Progress Notifications
If you want to capture and forward long-running progress indicators from an MCP server to an external logging dashboard, you can define an explicit route, and Luotsi will let it pass:

```yaml
nodes:
  - id: my_mcp_service
    is_mcp_server: true
    # ... runtime ...
    routes:
      - trigger: "notifications/progress"
        target: "my_dashboard_node"
      # All other notifications (like cancelled) are natively absorbed!
```

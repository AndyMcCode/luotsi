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

To prevent naming collisions when aggregating tools or prompts from multiple MCP servers, Luotsi automatically namespaces the discovered items by prefixing their names with the server's ID.

For example, if an MCP server with ID `odoo_mcp` exposes a tool named `search_customers`, Luotsi will cache and expose it to clients as `odoo_mcp__search_customers`.

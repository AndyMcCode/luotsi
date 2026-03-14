# Luotsi

"Luotsi" is Finnish for **Pilot**—the specialist who guides ships safely into harbor.

**What is it?**
Luotsi is a runtime for your AI agents. It acts as a secure layer between your code and the outside world (databases, APIs, tools).

**What problem does it solve?**
Building agents is easy; integrating them safely is hard. You usually end up writing boilerplate to connect to databases, handle API keys, and worry about your agent executing a `DROP TABLE` command.

### How it works
1.  **You write the Agent** focusing purely on logic, communicating via the **ACP (Agent Core Protocol)**.
2.  **You define the "Wiring" and "Policy"** in a config file, mapping **Ports** to specific **Adapters** (e.g., "Connect the AgentPort to the JsonRpcTcpAdapter").
3.  **Luotsi provides native, first-class support for the Model Context Protocol (MCP). Rather than just passing MCP messages opaquely, the Luotsi Core Runtime actively participates in the protocol via the `luotsi::ports::McpPort` to provide auto-discovery, capability caching, and intelligent routing.
**Key Benefits**
*   **Decoupled Architecture**: Use the **Ports and Adapters** pattern to swap transport technologies (local process, network service, database) without changing agent code.
*   **ACP/A2A Standardization**: Standardized protocols for Agent-to-Core and Agent-to-Agent communication.
*   **Policy Enforcement**: Define granular permissions at the boundary. If an agent tries to step out of bounds, Luotsi blocks it.
*   **Traceability**: Every tool call and message is logged. You get a complete audit trail of exactly what your agent did and why.

It is a simplified integration layer that keeps your agents useful, harmless, and accountable.

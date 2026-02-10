# Luotsi

"Luotsi" is Finnish for **Pilot**—the specialist who guides ships safely into harbor.

**What is it?**
Luotsi is a runtime for your AI agents. It acts as a secure layer between your code and the outside world (databases, APIs, tools).

**What problem does it solve?**
Building agents is easy; integrating them safely is hard. You usually end up writing boilerplate to connect to databases, handle API keys, and worry about your agent executing a `DROP TABLE` command.

**How it works**
1.  **You write the Agent** (in Python, JS, etc.) focusing purely on logic.
2.  **You define the "Wiring" and "Policy"** in a config file (e.g., "This agent can read from Postgres but only write to Slack").
3.  **Luotsi runs the system**, routing messages, enforcing rules, and logging every action.

**Key Benefits**
*   **Policy Enforcement**: Define granular permissions (e.g., "read-only access to Customer DB"). If an agent tries to step out of bounds, Luotsi blocks it.
*   **Traceability**: Every tool call and message is logged. You get a complete audit trail of exactly what your agent did and why.
*   **Safety**: Separation of concerns means your agent's logic is isolated from your infrastructure.

It is a simplified integration layer that keeps your agents useful, harmless, and accountable.

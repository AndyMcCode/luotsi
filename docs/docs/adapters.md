# Adapters

Adapters are responsible for the raw byte-level transport between the Luotsi Core and external nodes. They implement the [`IAdapter`](file:///home/andy/code/luotsi/luotsi-core/src/adapters/adapter.hpp) interface and sit below the [Ports layer](./ports.md), which normalizes their output into typed `MessageFrame` objects.

Available adapters:
- [Stdio Adapter](#stdio-adapter-the-docker-pilot)
- [JSON-RPC TCP Adapter](./architecture.md#coordinated-startup-dependency-orchestration)

---

## Stdio Adapter ("The Docker Pilot")

The **Stdio Adapter** is the primary interface for managing local child processes, especially **Docker Containers**, within the Luotsi runtime.

## Concept
Instead of implementing complex native Docker API calls in the Core, Luotsi treats Docker containers as simple processes spawned via the CLI (`docker run ...`). The Stdio Adapter manages the `stdin` (input) and `stdout` (output) of these processes to exchange messages.

## Configuration
To use the Stdio Adapter, set the `adapter` field to `stdio` in your `node.yaml` or global config.

```yaml
nodes:
  - id: "my_agent"
    runtime:
      adapter: "stdio"
      command: "docker"
      args: 
        - "run"
        - "-i"             # Interactive mode is crucial for stdin!
        - "--rm"           # Clean up container after exit
        - "my-docker-image"
```

## Protocol (Line-Delimited JSON)
The Adapter expects the child process to communicate using **JSON** messages, separated by newlines (`\n`).

### Ingress (Child -> Core)
When your agent prints to `stdout`:
```json
{"jsonrpc": "2.0", "method": "messaging.send", "params": {"to": "+12345"}}
```
The Adapter reads this line, wraps it in a Lux message frame, and injects it into the Core Bus.

### Egress (Core -> Child)
When the Core routes a message to your agent, the Adapter writes it to the child's `stdin`:
```json
{"jsonrpc": "2.0", "method": "messaging.receive", "params": {"from": "+98765", "text": "Hello"}}
```

### Role Delegation Metadata (`__luotsi_role__`)
The Stdio Adapter bi-directionally handles Role Delegation metadata:
- **Ingress**: If a child process includes `__luotsi_role__` in its outgoing JSON, the adapter extracts it into the message frame for the Core Policy Engine to process (requires the sender to be `is_trusted`).
- **Egress**: If a message arriving at the adapter has an associated `delegated_role`, the adapter re-injects it into the JSON payload as `__luotsi_role__`, allowing the child process to maintain security context.

## Best Practices
1.  **Always use `-i`**: If you forget `-i` in `docker run`, the container's stdin closes immediately, and the adapter cannot send messages to it.
2.  **Use `--rm`**: Let the Docker daemon clean up stopped containers to prevent resource leaks.
3.  **Flush Stdout**: Ensure your application (Python, Node, etc.) flushes `stdout` after every message, or disable buffering (e.g., `PYTHONUNBUFFERED=1`).
4.  **Logging**: Do NOT print logs to `stdout`! Use `stderr` for logs. The Stdio Adapter natively captures the child process's `stderr` stream asynchronously and automatically forwards it into the central `spdlog` engine, ensuring comprehensive, interwoven traceability of your agent's execution without polluting the JSON data plane on `stdout`.

---

## See Also

- [Ports Layer](./ports.md)
- [Architecture Overview](./architecture.md)
- [Routing](./routing.md)

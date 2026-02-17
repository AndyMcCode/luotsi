# Stdio Adapter ("The Docker Pilot")

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

## Best Practices
1.  **Always use `-i`**: If you forget `-i` in `docker run`, the container's stdin closes immediately, and the adapter cannot send messages to it.
2.  **Use `--rm`**: Let the Docker daemon clean up stopped containers to prevent resource leaks.
3.  **Flush Stdout**: Ensure your application (Python, Node, etc.) flushes `stdout` after every message, or disable buffering (e.g., `PYTHONUNBUFFERED=1`).
4.  **Logging**: Do NOT print logs to `stdout`! Use `stderr` for logs. `stdout` is exclusively for the data plane (JSON messages).

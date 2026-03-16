# Payload Guard: Real-time Message Sanitization

The **Payload Guard** is a core governance feature of Luotsi that protects agents and the runtime from oversized data blobs (e.g., base64 images, massive JSON responses) that could exceed LLM context windows or cause memory issues.

## 1. Overview

The guard operates as a "Deep Packet Inspection" layer within the message bus. Every message passing through the Core is recursively scanned before delivery.

## 2. Hierarchical Enforcement

Luotsi calculates the effective `max_token_size` (measured in characters for string fields) based on the sender's identity:

1.  **Global Default**: Specified in the main configuration file.
2.  **Policy Override**: Can be defined per-role in the policies file. If an authenticated agent has a role with a `max_token_size` defined, that limit takes precedence.

## 3. Implementation Details

The C++ Implementation (`Runtime::sanitize_payload`) performs the following:

- **Recursive Traversal**: Scans objects, arrays, and nested structures.
- **String Filtering**: Any string value whose length exceeds the calculated limit is replaced with the sentinel string: `"[BLOCKED: Field exceeds max_token_size]"`.
- **Latency Impact**: uses in-place JSON manipulation to maintain sub-millisecond routing overhead.

## 4. Configuration Examples

### Global Limit (`langchain_odoo.config.yaml`)
```yaml
max_token_size: 10000
```

### Role-specific Override (`policies.yaml`)
```yaml
roles:
  - name: "guest"
    secret_key: "guest_key_123"
    max_token_size: 5000  # Stricter limit for guests
```

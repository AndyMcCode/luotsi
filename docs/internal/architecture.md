# Architecture Internals (Engine Room)

This document describes the internal C++ design of the Luotsi Core "Walking Skeleton".

## The Big Picture
Luotsi is a single-threaded (mostly) asynchronous event loop built on top of `asio`. It follows a **Hexagonal Architecture** where the `Runtime` sits at the center, and `Adapters` form the ports to the outside world. It supports **Hot Reloading** via `SIGHUP`, dynamically reconciling the active adapters with the new configuration.

```mermaid
graph TD
    Config[Config Loader] --> Runtime
    Runtime --> Bus[The Bus / Loop]
    Runtime --> Factory[Adapter Factory]
    Factory --> StdioAdapter
    Factory --> HttpAdapter[HttpAdapter (Future)]
    
    StdioAdapter <--> Process[Child Process / Docker]
```

## Core Components

### 1. `luotsi::Config` (`src/core/config.hpp`)
Handles loading parsing of the `luotsi.config.yaml` using `yaml-cpp`.
-   **Structs**: `NodeConfig`, `RuntimeConfig`, `RouteConfig`.
-   **Philosophy**: Pure data structs, no logic.

### 2. `luotsi::Runtime` (`src/core/runtime.hpp`)
The main application class.
-   **Responsibility**:
    1.  Owns `asio::io_context`.
    2.  Instantiates Adapters based on Config.
    3.  Maintains the global `adapters_` map.
    4.  **The Routing Logic**: Implements the `route_message` callback which acts as the "Bus".

### 3. `luotsi::IAdapter` (`src/adapters/adapter.hpp`)
Abstract interface for all adapters.
-   `init(config)`: Setup.
-   `start()`: Begin execution (e.g., `fork()`).
-   `send(frame)`: Acceptance of outbound messages.
-   `set_on_receive(cb)`: Injects messages into the core.

### 4. `luotsi::StdioAdapter` (`src/adapters/stdio_adapter.cpp`)
Implements process management.
-   **Spawn**: Uses `fork()` / `execvp()` / `dup2()` to redirect pipes.
-   **Async I/O**: Uses `asio::posix::stream_descriptor` to read from the child's `audio` pipe asynchronously without blocking the loop.
-   **Protocol**: Simple Line-Delimited JSON parsing using `nlohmann::json`.

## Routing & Governance (Phase 0.5)
Currently, routing is **Static** and **Prefix-based**.
-   **Table**: Defined in `routes` list of each node in YAML.
-   **Logic**:
    1.  Core receives message from Source `A`.
    2.  Core extracts `method` from JSON payload.
    3.  Core iterates `A`'s configured routes.
    4.  If `method` starts with `trigger`, forward to `target`.

## Future Improvements (Post-Skeleton)
-   **Dynamic Shared Libraries**: Load adapters as `.so` plugins.
-   **Zero-Copy**: Optimize message passing (though JSON copying dominates currently).
-   **Policy Engine**: Inspect full payload against Rego/OPA-like rules.

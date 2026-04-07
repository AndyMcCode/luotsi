# Development Guide

How to build, test, and run the Luotsi Core.

## Prerequisites
-   **C++ Compiler**: GCC 11+ or Clang 14+ (C++20 support required).
-   **CMake**: Version 3.14+.
-   **Tools**: `make`, `git`.

## Building
We use CMake with `FetchContent` for dependency management. No external package manager (conan/vcpkg) is required for the Skeleton phase.

```bash
cd luotsi-core
cmake -S . -B build
cmake --build build
```

The binary will be at `build/luotsi`.

## Running Tests
We use Google Test.

```bash
cd luotsi-core/build
./tests/luotsi_tests
```

## Running the Playground (PoC)
To run the Odoo <-> WhatsApp demo:

1.  **Build Docker Images**:
    ```bash
    cd playground/whatsapp && docker build -t luotsi-playground-whatsapp .
    cd ../odoo && docker build -t luotsi-playground-odoo .
    ```

3.  **Run Luotsi Core**:
    ```bash
    # From repo root
    ./luotsi-core/build/luotsi --config playground/configs/luotsi.config.yaml
    ```
    *(Note: You might need to adjust paths depending on where you run from. The config loader expects paths to be correct relative to CWD if they are files, but currently `node.yaml` structure isn't fully implemented in file-splitting, everything is in one mono-config for the skeleton.)*

## Advanced Playgrounds

The core repo contains multiple playground configurations to test advanced features:

- **Langchain & Odoo**: A demonstration of real-world tool execution and orchestration via Luotsi's `langchain-agent` client interacting with the Odoo MCP. Use `playground/configs/langchain_odoo.config.yaml`.
- **Multi-Node Orchestration**: Features a `dummy-mcp` server integrated alongside other tools to comprehensively validate `fan_out_mcp` multiplexing and `mcp_call_router` orchestration workflows without deploying heavy external dependencies.

## Debugging
-   **Logging**: Powered by `spdlog`. Check stdout.
-   **VSCode**: `.vscode` launch configurations are encouraged.

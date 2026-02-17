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

2.  **Run Luotsi**:
    ```bash
    # From repo root
    ./luotsi-core/build/luotsi --config playground/configs/luotsi.config.yaml
    ```
    *(Note: You might need to adjust paths depending on where you run from. The config loader expects paths to be correct relative to CWD if they are files, but currently `node.yaml` structure isn't fully implemented in file-splitting, everything is in one mono-config for the skeleton.)*

## Debugging
-   **Logging**: Powered by `spdlog`. Check stdout.
-   **VSCode**: `.vscode` launch configurations are encouraged.

# LUOTSI

**The Lifecycle Manager for Agentic Workflows.**

"Luotsi" (Finnish for *Pilot*) acts as the core engine and governance kernel for your agent ecosystem. It provides a robust runtime for managing the lifecycle, communication, and compliance of AI agents, tools, and services.

## What is Luotsi?

Luotsi is a "software-defined switch fabric" for agentic systems. It connects disparate components—Agents, MCP Servers, specialized Nodes—through a central core that manages:

*   **Routing**: Intelligently directs messages and tool calls between nodes based on declarative workflows.
*   **Lifecycle Management**: Orchestrates the startup, shutdown, and health monitoring of all connected processes.
*   **Compliance & Policy**: Enforces strict `allow/deny` governance rules on every message, ensuring safe and authorized interactions.
*   **Observability**: Provides deep visibility with structured logging and tracing for all agent activities.

## Core Architecture

The system follows a hexagonal architecture where the **Core** handles governance and routing, while **Adapters** connect to the outside world:

*   **The Core**: A high-performance C++ runtime acting as the central nervous system.
*   **Nodes**: The functional units (Agents, Databases, MCP Servers, UCP endpoints) that perform work.
*   **Adapters**: Protocol translators (Stdio, HTTP, TCP) that let any software plug into the core.
*   **Configuration**: Workflows and topologies are defined in simple YAML, allowing for dynamic "rewiring" without code changes.

## Key Features

*   **Universal Connectivity**: Connect agents to any interface via standardized adapters.
*   **Protocol Support**: Built-in handling for MCP (Model Context Protocol), UCP, and JSON-RPC.
*   **Zero-Trust Security**: Nodes are isolated by default; communication requires explicit policy approval.
*   **Language Agnostic**: Write your agents in Python, TypeScript, Go, or any language that speaks JSON.

## Getting Started

See the [Documentation Index](docs/index.md) for installation guides, architecture details, and tutorials.

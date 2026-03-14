#pragma once

#include <string>
#include <vector>
#include <map>
#include <optional>
#include <expected>
#include <yaml-cpp/yaml.h>

namespace luotsi::internal {

struct RuntimeConfig {
    std::string adapter;
    std::string command;
    std::vector<std::string> args;
    std::string host;
    int port = 0;
};

struct RouteConfig {
    std::string trigger; // e.g. "namespace" or "method"
    std::string target; // Node ID (for simple routing)
    std::vector<std::string> targets; // Node IDs (for fan_out_mcp)
    std::string action; // "forward", "translate", "fan_out_mcp", "mcp_call_router"
    std::string new_method; // The new method name if action == "translate"
};

struct NodeConfig {
    std::string id;
    bool is_mcp_server = false;
    std::vector<std::string> disabled_capabilities;
    RuntimeConfig runtime;
    std::vector<std::string> depends;
    std::vector<RouteConfig> routes;
    // Policies could go here later
};

struct PolicyRole {
    std::string name;
    std::string secret_key;
    std::vector<std::string> allowed_servers;
};

struct Config {
    std::string log_level = "info";
    std::optional<std::string> audit_log;
    std::optional<std::string> policies_file;
    std::vector<NodeConfig> nodes;

    static std::expected<Config, std::string> load_from_file(const std::string& path);
    static std::expected<std::vector<PolicyRole>, std::string> load_policies(const std::string& path);
};

} // namespace luotsi::internal

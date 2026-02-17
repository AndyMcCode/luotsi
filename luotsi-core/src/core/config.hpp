#pragma once

#include <string>
#include <vector>
#include <map>
#include <optional>
#include <expected>
#include <yaml-cpp/yaml.h>

namespace luotsi {

struct RuntimeConfig {
    std::string adapter;
    std::string command;
    std::vector<std::string> args;
    std::string host;
    int port = 0;
};

struct RouteConfig {
    std::string trigger; // e.g. "namespace" or "method"
    std::string target; // Node ID
    std::string action; // "forward", etc.
};

struct NodeConfig {
    std::string id;
    RuntimeConfig runtime;
    std::vector<RouteConfig> routes;
    // Policies could go here later
};

struct Config {
    std::string log_level = "info";
    std::optional<std::string> audit_log;
    std::vector<NodeConfig> nodes;

    static std::expected<Config, std::string> load_from_file(const std::string& path);
};

} // namespace luotsi

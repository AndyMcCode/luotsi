#include "config.hpp"
#include <fstream>
#include <stdexcept>
#include <spdlog/spdlog.h>

namespace luotsi::internal {

std::expected<Config, std::string> Config::load_from_file(const std::string& path) {
    Config config;
    try {
        YAML::Node root = YAML::LoadFile(path);

        if (root["log_level"]) {
            config.log_level = root["log_level"].as<std::string>();
        }

        if (root["audit_log"]) {
            config.audit_log = root["audit_log"].as<std::string>();
        }
        
        if (root["policies_file"]) {
            config.policies_file = root["policies_file"].as<std::string>();
        }

        if (root["max_token_size"]) {
            config.max_token_size = root["max_token_size"].as<size_t>();
        }


        if (root["nodes"]) {
            for (const auto& node_yaml : root["nodes"]) {
                NodeConfig node_config;
                node_config.id = node_yaml["id"].as<std::string>();

                if (node_yaml["runtime"]) {
                    node_config.runtime.adapter = node_yaml["runtime"]["adapter"].as<std::string>();
                    if (node_yaml["runtime"]["command"]) {
                        node_config.runtime.command = node_yaml["runtime"]["command"].as<std::string>();
                    }
                    if (node_yaml["runtime"]["args"]) {
                        for (const auto& arg : node_yaml["runtime"]["args"]) {
                            node_config.runtime.args.push_back(arg.as<std::string>());
                        }
                    }
                    if (node_yaml["runtime"]["host"]) {
                        node_config.runtime.host = node_yaml["runtime"]["host"].as<std::string>();
                    }
                    if (node_yaml["runtime"]["port"]) {
                        node_config.runtime.port = node_yaml["runtime"]["port"].as<int>();
                    }
                }

                if (node_yaml["is_mcp_server"]) {
                    node_config.is_mcp_server = node_yaml["is_mcp_server"].as<bool>();
                }
                if (node_yaml["master"]) {
                    node_config.master = node_yaml["master"].as<bool>();
                }
                if (node_yaml["session_memory"]) {
                    node_config.session_memory = node_yaml["session_memory"].as<bool>();
                }
                
                if (node_yaml["disabled_capabilities"]) {
                    for (const auto& cap : node_yaml["disabled_capabilities"]) {
                        node_config.disabled_capabilities.push_back(cap.as<std::string>());
                    }
                }

                if (node_yaml["depends"]) {
                    for (const auto& dep : node_yaml["depends"]) {
                        node_config.depends.push_back(dep.as<std::string>());
                    }
                }

                if (node_yaml["routes"]) {
                    for (const auto& route_yaml : node_yaml["routes"]) {
                        RouteConfig route_config;
                        if (route_yaml["namespace"]) {
                             route_config.trigger = route_yaml["namespace"].as<std::string>();
                        } else if (route_yaml["trigger"]) {
                             route_config.trigger = route_yaml["trigger"].as<std::string>();
                        }
                        
                        if (route_yaml["target"]) {
                            route_config.target = route_yaml["target"].as<std::string>();
                        }
                        if (route_yaml["targets"]) {
                            for (const auto& target : route_yaml["targets"]) {
                                route_config.targets.push_back(target.as<std::string>());
                            }
                        }
                        if (route_yaml["action"]) {
                            route_config.action = route_yaml["action"].as<std::string>();
                        }
                        if (route_yaml["new_method"]) {
                            route_config.new_method = route_yaml["new_method"].as<std::string>();
                        }
                        node_config.routes.push_back(route_config);
                    }
                }

                config.nodes.push_back(node_config);
            }
        }
    } catch (const YAML::Exception& e) {
        // C++23: Using std::unexpected
        return std::unexpected(std::string("Failed to parse config file ") + path + ": " + e.what());
    } catch (const std::exception& e) {
        return std::unexpected(std::string("Failed to load config file ") + path + ": " + e.what());
    }

    return config;
}

std::expected<std::vector<PolicyRole>, std::string> Config::load_policies(const std::string& path) {
    try {
        YAML::Node root = YAML::LoadFile(path);
        std::vector<PolicyRole> roles;

        if (root["roles"]) {
            for (const auto& role_yaml : root["roles"]) {
                PolicyRole role;
                if (role_yaml["name"]) role.name = role_yaml["name"].as<std::string>();
                if (role_yaml["secret_key"]) role.secret_key = role_yaml["secret_key"].as<std::string>();
                if (role_yaml["allowed_servers"]) {
                    for (const auto& srv : role_yaml["allowed_servers"]) {
                        role.allowed_servers.push_back(srv.as<std::string>());
                    }
                }
                if (role_yaml["max_token_size"]) {
                    role.max_token_size = role_yaml["max_token_size"].as<size_t>();
                }
                if (role_yaml["is_trusted"]) {
                    role.is_trusted = role_yaml["is_trusted"].as<bool>();
                }
                if (role_yaml["allowed_tools"]) {
                    for (const auto& t : role_yaml["allowed_tools"]) role.allowed_tools.push_back(t.as<std::string>());
                }
                if (role_yaml["blocked_tools"]) {
                    for (const auto& t : role_yaml["blocked_tools"]) role.blocked_tools.push_back(t.as<std::string>());
                }
                if (role_yaml["allowed_resources"]) {
                    for (const auto& r : role_yaml["allowed_resources"]) role.allowed_resources.push_back(r.as<std::string>());
                }
                if (role_yaml["blocked_resources"]) {
                    for (const auto& r : role_yaml["blocked_resources"]) role.blocked_resources.push_back(r.as<std::string>());
                }
                roles.push_back(role);
            }
        }
        return roles;

    } catch (const YAML::Exception& e) {
        return std::unexpected(std::string("YAML Exception loading policies: ") + e.what());
    }
}

bool wildcard_match(const std::string& pattern, const std::string& text) {
    if (pattern == "*") return true;
    if (pattern.empty()) return text.empty();
    
    // Simple suffix wildcard: "foo:*"
    if (pattern.back() == '*') {
        std::string prefix = pattern.substr(0, pattern.size() - 1);
        return text.compare(0, prefix.size(), prefix) == 0;
    }
    
    // Exact match
    return pattern == text;
}

} // namespace luotsi::internal

#include "config.hpp"
#include <fstream>
#include <stdexcept>
#include <spdlog/spdlog.h>

namespace luotsi {

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

                if (node_yaml["routes"]) {
                    for (const auto& route_yaml : node_yaml["routes"]) {
                        RouteConfig route_config;
                        if (route_yaml["namespace"]) {
                             route_config.trigger = route_yaml["namespace"].as<std::string>();
                        } else if (route_yaml["trigger"]) {
                             route_config.trigger = route_yaml["trigger"].as<std::string>();
                        }
                        
                        route_config.target = route_yaml["target"].as<std::string>();
                        
                        if (route_yaml["action"]) {
                            route_config.action = route_yaml["action"].as<std::string>();
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

} // namespace luotsi

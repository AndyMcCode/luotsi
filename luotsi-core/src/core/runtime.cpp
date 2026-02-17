#include "runtime.hpp"
#include <spdlog/spdlog.h>
#include "../adapters/stdio_adapter.hpp"
#include "../adapters/jsonrpc_tcp_adapter.hpp"

namespace luotsi {

// Equality operators for config structs to detect changes
bool operator==(const RuntimeConfig& a, const RuntimeConfig& b) {
    return a.adapter == b.adapter &&
           a.command == b.command &&
           a.args == b.args &&
           a.host == b.host &&
           a.port == b.port;
}

bool operator!=(const RuntimeConfig& a, const RuntimeConfig& b) {
    return !(a == b);
}

Runtime::Runtime(const std::string& config_path) 
    : config_path_(config_path), signals_(io_context_) {
    
    // Initial Load
    reload_config();
}

void Runtime::await_signal() {
    signals_.async_wait([this](const std::error_code& error, int signal_number) {
        if (!error) {
            spdlog::info("Signal handler caught signal: {}", signal_number);
            if (signal_number == SIGHUP) {
                spdlog::info("Received SIGHUP, reloading config...");
                reload_config();
                // Re-register wait
                await_signal(); 
            } else {
                 spdlog::info("Received signal {}, shutting down...", signal_number);
                 stop();
            }
        } else {
            spdlog::error("Signal wait error: {}", error.message());
        }
    });
}

void Runtime::start() {
    spdlog::info("Luotsi Core starting...");

    // Register Signal Handler (SIGHUP for reload, SIGINT/TERM for shutdown)
    signals_.add(SIGHUP);
    signals_.add(SIGINT);
    signals_.add(SIGTERM);
    
    await_signal();

    io_context_.run();
}

void Runtime::stop() {
    spdlog::info("Stopping Runtime...");
    for (auto& [id, adapter] : adapters_) {
        adapter->stop();
    }
    io_context_.stop();
}

void Runtime::reload_config() {
    auto result = Config::load_from_file(config_path_);
    if (!result) {
        spdlog::error("Failed to load config: {}", result.error());
        return;
    }
    
    Config new_config = result.value();
    spdlog::set_level(spdlog::level::from_str(new_config.log_level));
    spdlog::info("Config loaded from {}", config_path_);

    // Init or Update Observability
    if (new_config.audit_log) {
        if (!observability_) {
            observability_ = std::make_unique<Observability>(*new_config.audit_log);
        } 
        // Note: Changing audit log path at runtime not supported yet without more logic
    }

    reconcile_adapters(new_config);
    config_ = new_config; // Swap
}

void Runtime::reconcile_adapters(const Config& new_config) {
    std::map<std::string, const NodeConfig*> new_nodes_map;
    for (const auto& node : new_config.nodes) {
        new_nodes_map[node.id] = &node;
    }

    // 1. Identify Removed Nodes or Changed Runtimes
    for (auto it = adapters_.begin(); it != adapters_.end();) {
        std::string id = it->first;
        auto new_it = new_nodes_map.find(id);

        bool remove = false;
        if (new_it == new_nodes_map.end()) {
            spdlog::info("Node '{}' removed from config. Stopping...", id);
            remove = true;
        } else {
             // Check if Runtime config changed
            // Find old node config
            const NodeConfig* old_node = nullptr;
            for(const auto& n : config_.nodes) { if(n.id == id) { old_node = &n; break; } }
            
            if (old_node && old_node->runtime != new_it->second->runtime) {
                spdlog::info("Node '{}' runtime config changed. Restarting...", id);
                remove = true; 
            }
        }

        if (remove) {
            it->second->stop();
            it = adapters_.erase(it);
        } else {
            ++it;
        }
    }

    // 2. Start New Nodes or Restarted Nodes
    for (const auto& node_cfg : new_config.nodes) {
        if (adapters_.find(node_cfg.id) == adapters_.end()) {
            spdlog::info("Starting new node '{}'...", node_cfg.id);
            std::shared_ptr<IAdapter> adapter;

            if (node_cfg.runtime.adapter == "stdio") {
                adapter = std::make_shared<StdioAdapter>(io_context_, node_cfg.id);
            } else if (node_cfg.runtime.adapter == "jsonrpc_tcp") {
                adapter = std::make_shared<JsonRpcTcpAdapter>(io_context_, node_cfg.id);
            } else {
                spdlog::warn("Unknown adapter '{}'", node_cfg.runtime.adapter);
                continue;
            }

            adapter->init(node_cfg.runtime);
            
            // Wire up Bus with ID capture (late binding lookup)
            std::string my_id = node_cfg.id;
            adapter->set_on_receive([this, my_id](MessageFrame frame) {
                this->route_message(frame, my_id);
            });

            adapter->start();
            adapters_[node_cfg.id] = adapter;
        }
    }
}

void Runtime::route_message(MessageFrame& frame, const std::string& source_id) {
    // Lookup current config for this source
    const NodeConfig* source_config = nullptr;
    for(const auto& node : config_.nodes) {
        if(node.id == source_id) {
            source_config = &node;
            break;
        }
    }

    if (!source_config) {
        spdlog::warn("Received message from unknown/removed node '{}'. Dropping.", source_id);
        return;
    }

    if (observability_) {
        observability_->log_message(frame);
    }

    spdlog::info("Bus received from {}: {}", source_id, frame.payload.dump());

    // 1. Response Routing
    if (frame.payload.contains("id") && !frame.payload.contains("method")) {
        std::string id_str = frame.payload["id"].dump();
        auto it = pending_requests_.find(id_str);
        if (it != pending_requests_.end()) {
            std::string target = it->second;
            // Validate target still exists
            auto target_it = adapters_.find(target);
            if (target_it != adapters_.end()) {
                spdlog::info("Auto-routing Response {} -> {}", source_id, target);
                frame.target_id = target;
                target_it->second->send(frame);
                pending_requests_.erase(it);
                return;
            }
        }
    }

    // 2. Request Routing
    std::string method;
    if (frame.payload.contains("method")) {
        method = frame.payload["method"].get<std::string>();
        if (frame.payload.contains("id")) {
             pending_requests_[frame.payload["id"].dump()] = source_id;
        }
    }

    for (const auto& route : source_config->routes) {
        if (!method.empty() && method.find(route.trigger) == 0) {
            auto target_it = adapters_.find(route.target);
            if (target_it != adapters_.end()) {
                spdlog::info("Routing Request {} -> {}", source_id, route.target);
                frame.target_id = route.target;
                target_it->second->send(frame);
            } else {
                spdlog::error("Route target '{}' not found", route.target);
            }
            return;
        }
    }
    
    spdlog::warn("No route found for message from {} (method: {})", source_id, method);
}

} // namespace luotsi

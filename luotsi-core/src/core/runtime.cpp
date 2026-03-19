#include "runtime.hpp"
#include <spdlog/spdlog.h>
#include "../adapters/stdio_adapter.hpp"
#include "../adapters/jsonrpc_tcp_adapter.hpp"

namespace luotsi::internal {

using namespace luotsi::adapters;

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
    for (auto& [id, port] : ports_) {
        // Technically we should stop the adapter via the port if we add a stop() to IPort
        // or just let the shared_ptr cleanup handle it. 
        // For now, assume ports don't have stop but we want to stop adapters.
    }
    // Since ports_ shares pointers with the adapters, we'll need a way to reach them or just clear ports.
    ports_.clear(); 
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
    // Scan for session_memory and master flags
    session_memory_node_id_.clear();
    for (const auto& node : new_config.nodes) {
        if (node.session_memory) {
            session_memory_node_id_ = node.id;
            spdlog::info("Session memory node configured: '{}'", node.id);
        }
    }

    // Init or Update Observability
    if (new_config.audit_log) {
        if (!observability_) {
            observability_ = std::make_unique<Observability>(*new_config.audit_log);
        } 
        // Note: Changing audit log path at runtime not supported yet without more logic
    }
    
    // Load Policies
    roles_.clear();
    if (new_config.policies_file) {
        auto policy_result = Config::load_policies(*new_config.policies_file);
        if (policy_result) {
            roles_ = policy_result.value();
            spdlog::info("Loaded {} policy roles from {}", roles_.size(), *new_config.policies_file);
        } else {
            spdlog::error("Failed to load policies: {}", policy_result.error());
        }
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
    for (auto it = ports_.begin(); it != ports_.end();) {
        std::string id = it->first;
        auto new_it = new_nodes_map.find(id);

        bool remove = false;
        if (new_it == new_nodes_map.end()) {
            spdlog::info("Node '{}' removed from config. Removing port...", id);
            remove = true;
        } else {
             // Check if Runtime config changed
            const NodeConfig* old_node = nullptr;
            for(const auto& n : config_.nodes) { if(n.id == id) { old_node = &n; break; } }
            
            if (old_node && old_node->runtime != new_it->second->runtime) {
                spdlog::info("Node '{}' runtime config changed. Restarting...", id);
                remove = true; 
            }
        }

        if (remove) {
            it = ports_.erase(it);
        } else {
            ++it;
        }
    }

    // 2. Start New Nodes or Restarted Nodes
    master_node_id_.clear(); // reset on each reconcile
    for (const auto& node_cfg : new_config.nodes) {
        if (node_cfg.master) {
            if (master_node_id_.empty()) {
                master_node_id_ = node_cfg.id;
                spdlog::info("Master node configured: '{}'", master_node_id_);
            } else {
                spdlog::error("Multiple master nodes defined! Only '{}' will be used. Ignoring '{}'.", master_node_id_, node_cfg.id);
            }
        }
        if (ports_.find(node_cfg.id) == ports_.end()) {
            if (is_dependency_satisfied(node_cfg, new_config)) {
                spawn_node(node_cfg, new_config);
                // Check if other deferred nodes can now start
                check_deferred_nodes(new_config);
            } else {
                spdlog::info("Node '{}' dependency not satisfied. Deferring startup...", node_cfg.id);
                deferred_nodes_.push_back(node_cfg.id);
            }
        }
    }
}

void Runtime::spawn_node(const NodeConfig& node_cfg, const Config& current_config) {
    if (ports_.find(node_cfg.id) != ports_.end()) return;

    spdlog::info("Starting node '{}' via requested adapter...", node_cfg.id);
    std::shared_ptr<IAdapter> adapter;

    if (node_cfg.runtime.adapter == "stdio") {
        adapter = std::make_shared<StdioAdapter>(io_context_, node_cfg.id);
    } else if (node_cfg.runtime.adapter == "jsonrpc_tcp") {
        adapter = std::make_shared<JsonRpcTcpAdapter>(io_context_, node_cfg.id);
    } else {
        spdlog::warn("Unknown adapter '{}'", node_cfg.runtime.adapter);
        return;
    }

    adapter->init(node_cfg.runtime);
    
    // Create the appropriate Port
    std::shared_ptr<ports::IPort> port;
    if (node_cfg.is_mcp_server) {
        port = std::make_shared<ports::GenericMcpPort>(node_cfg.id, adapter);
    } else {
        port = std::make_shared<ports::GenericAgentPort>(node_cfg.id, adapter);
    }

    // Wire up Port to Core (Inbound)
    std::string my_id = node_cfg.id;
    port->set_on_receive([this, my_id](MessageFrame frame) {
        this->route_message(frame, my_id);
    });

    adapter->start();
    ports_[node_cfg.id] = port;

    if (node_cfg.is_mcp_server) {
        spdlog::info("Initiating auto-discovery handshake with MCP port '{}'", node_cfg.id);
        MessageFrame init_frame;
        init_frame.source_id = "luotsi-hub";
        init_frame.target_id = node_cfg.id;
        init_frame.payload = {
            {"jsonrpc", "2.0"},
            {"id", "__luotsi__init__" + node_cfg.id},
            {"method", "initialize"},
            {"params", {
                {"protocolVersion", "2024-11-05"},
                {"capabilities", nlohmann::json::object()},
                {"clientInfo", {{"name", "luotsi-hub"}, {"version", "1.0.0"}}}
            }}
        };
        port->send(init_frame);
    }
}

bool Runtime::is_dependency_satisfied(const NodeConfig& node, const Config& current_config) {
    if (node.depends.empty()) return true;
    
    for (const auto& dep_id : node.depends) {
        // Dependency must have a port
        auto it = ports_.find(dep_id);
        if (it == ports_.end()) {
            return false;
        }
        
        // If dependency is an MCP server, it must be initialized
        const NodeConfig* dep_cfg = nullptr;
        for (const auto& n : current_config.nodes) {
            if (n.id == dep_id) { dep_cfg = &n; break; }
        }
        
        if (dep_cfg && dep_cfg->is_mcp_server) {
            auto mcp_port = std::dynamic_pointer_cast<ports::McpPort>(it->second);
            if (mcp_port && !mcp_port->isInitialized()) {
                return false;
            }
        }
    }
    return true;
}

void Runtime::check_deferred_nodes(const Config& current_config) {
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto it = deferred_nodes_.begin(); it != deferred_nodes_.end(); ) {
            const std::string& node_id = *it;
            const NodeConfig* node_cfg = nullptr;
            for (const auto& n : current_config.nodes) {
                if (n.id == node_id) { node_cfg = &n; break; }
            }

            if (node_cfg && is_dependency_satisfied(*node_cfg, current_config)) {
                spawn_node(*node_cfg, current_config);
                it = deferred_nodes_.erase(it);
                changed = true;
            } else {
                ++it;
            }
        }
    }
}

void Runtime::route_message(luotsi::MessageFrame& frame, const std::string& source_id) {
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

    // 0. Payload Guard (Hierarchical)
    size_t effective_limit = config_.max_token_size;
    std::string active_role_name = get_active_role_name(frame, source_id);

    if (!active_role_name.empty()) {
        // Apply limits for the active role (assigned or delegated)
        for (const auto& role : roles_) {
            if (role.name == active_role_name && role.max_token_size.has_value()) {
                effective_limit = role.max_token_size.value();
                break;
            }
        }
    }
    sanitize_payload(frame.payload, effective_limit);

    // 0.1 Tool Execution Check
    std::string method;
    if (frame.payload.contains("method")) {
        method = frame.payload["method"].get<std::string>();
        
        if (method == "tools/call") {
             std::string full_name = frame.payload["params"]["name"];
             // Translate Luotsi internal __ to policy : for checking
             std::string policy_name = full_name;
             size_t pos = policy_name.find("__");
             if (pos != std::string::npos) {
                 policy_name.replace(pos, 2, ":");
             }

             if (!is_tool_allowed(active_role_name, policy_name)) {
                 spdlog::warn("Access Denied: Role '{}' attempted to call unauthorized tool '{}' (policy check: '{}')", active_role_name, full_name, policy_name);
                 
                 MessageFrame error_reply;
                 error_reply.source_id = "luotsi-hub";
                 error_reply.target_id = source_id;
                 error_reply.payload = {
                     {"jsonrpc", "2.0"},
                     {"id", frame.payload["id"]},
                     {"error", {{"code", -32001}, {"message", "Access Denied: Unauthorized tool call"}}}
                 };
                 ports_[source_id]->send(error_reply);
                 return;
             }
        } else if (method == "resources/read") {
             std::string uri = frame.payload["params"]["uri"];
             if (!is_resource_allowed(active_role_name, uri)) {
                 spdlog::warn("Access Denied: Role '{}' attempted to read unauthorized resource '{}'", active_role_name, uri);
                 
                 MessageFrame error_reply;
                 error_reply.source_id = "luotsi-hub";
                 error_reply.target_id = source_id;
                 error_reply.payload = {
                     {"jsonrpc", "2.0"},
                     {"id", frame.payload["id"]},
                     {"error", {{"code", -32001}, {"message", "Access Denied: Unauthorized resource access"}}}
                 };
                 ports_[source_id]->send(error_reply);
                 return;
             }
        }
    }

    // 1. Response Routing
    if (frame.payload.contains("id") && !frame.payload.contains("method")) {
        std::string global_id_str = frame.payload["id"].is_string() ? frame.payload["id"].get<std::string>() : frame.payload["id"].dump();
        
        // Check for internal auto-discovery responses
        if (global_id_str.find("__luotsi__") == 0) {
            spdlog::info("Intercepted internal discovery response from {}: {}", source_id, global_id_str);
            
            auto mcp_port = std::dynamic_pointer_cast<ports::McpPort>(ports_[source_id]);
            if (!mcp_port) return;

            if (global_id_str == "__luotsi__init__" + source_id) {
                MessageFrame notif;
                notif.source_id = "luotsi-hub";
                notif.target_id = source_id;
                notif.payload = { {"jsonrpc", "2.0"}, {"method", "notifications/initialized"} };
                mcp_port->send(notif);
                
                auto is_disabled = [&](const std::string& cap) {
                    if (source_config) {
                        for (const auto& d : source_config->disabled_capabilities) {
                            if (d == cap) return true;
                        }
                    }
                    return false;
                };

                if (!is_disabled("tools/list")) {
                    MessageFrame req;
                    req.source_id = "luotsi-hub";
                    req.target_id = source_id;
                    req.payload = { {"jsonrpc", "2.0"}, {"id", "__luotsi__tools__" + source_id}, {"method", "tools/list"} };
                    mcp_port->send(req);
                }

                if (!is_disabled("resources/list")) {
                    MessageFrame req2;
                    req2.source_id = "luotsi-hub";
                    req2.target_id = source_id;
                    req2.payload = { {"jsonrpc", "2.0"}, {"id", "__luotsi__resources__" + source_id}, {"method", "resources/list"} };
                    mcp_port->send(req2);
                }
                
                if (!is_disabled("resources/templates/list")) {
                    MessageFrame req3;
                    req3.source_id = "luotsi-hub";
                    req3.target_id = source_id;
                    req3.payload = { {"jsonrpc", "2.0"}, {"id", "__luotsi__templates__" + source_id}, {"method", "resources/templates/list"} };
                    mcp_port->send(req3);
                }
                
                if (!is_disabled("prompts/list")) {
                    MessageFrame req4;
                    req4.source_id = "luotsi-hub";
                    req4.target_id = source_id;
                    req4.payload = { {"jsonrpc", "2.0"}, {"id", "__luotsi__prompts__" + source_id}, {"method", "prompts/list"} };
                    mcp_port->send(req4);
                }
                return;
            } else if (global_id_str == "__luotsi__tools__" + source_id) {
                if (frame.payload.contains("result") && frame.payload["result"].contains("tools")) {
                    nlohmann::json tools = frame.payload["result"]["tools"];
                    nlohmann::json namespaced_tools = nlohmann::json::array();
                    for (auto& tool : tools) {
                        std::string original_name = tool["name"];
                        tool["name"] = source_id + "__" + original_name;
                        namespaced_tools.push_back(tool);
                    }
                    mcp_port->updateCapabilities("tools", namespaced_tools);
                    spdlog::info("Cached {} tools for {}", tools.size(), source_id);
                }
                return;
            } else if (global_id_str == "__luotsi__resources__" + source_id) {
                if (frame.payload.contains("result") && frame.payload["result"].contains("resources")) {
                    mcp_port->updateCapabilities("resources", frame.payload["result"]["resources"]);
                    spdlog::info("Cached {} resources for {}", frame.payload["result"]["resources"].size(), source_id);
                }
                return;
            } else if (global_id_str == "__luotsi__templates__" + source_id) {
                if (frame.payload.contains("result") && frame.payload["result"].contains("resourceTemplates")) {
                    mcp_port->updateCapabilities("templates", frame.payload["result"]["resourceTemplates"]);
                    spdlog::info("Cached {} templates for {}", frame.payload["result"]["resourceTemplates"].size(), source_id);
                }
                return;
            } else if (global_id_str == "__luotsi__prompts__" + source_id) {
                if (frame.payload.contains("result") && frame.payload["result"].contains("prompts")) {
                    nlohmann::json prompts = frame.payload["result"]["prompts"];
                    nlohmann::json namespaced_prompts = nlohmann::json::array();
                    for (auto& pr : prompts) {
                        std::string original_name = pr["name"];
                        pr["name"] = source_id + "__" + original_name;
                        namespaced_prompts.push_back(pr);
                    }
                    mcp_port->updateCapabilities("prompts", namespaced_prompts);
                    spdlog::info("Cached {} prompts for {}", prompts.size(), source_id);
                }
                mcp_port->markInitialized(true); 
                check_deferred_nodes(config_);
                return;
            }
        }

        // Check for normal request/response tracking
        auto it = pending_requests_.find(global_id_str);
        if (it != pending_requests_.end()) {
            std::string target = it->second;
            // Validate target still exists
            auto target_port_it = ports_.find(target);
            if (target_port_it != ports_.end()) {
                spdlog::info("Auto-routing Response {} -> {}", source_id, target);
                
                // Un-NAT the ID (Restore original ID)
                auto orig_it = original_ids_.find(global_id_str);
                if (orig_it != original_ids_.end()) {
                    frame.payload["id"] = orig_it->second;
                    original_ids_.erase(orig_it);
                }

                // Fork to session memory if configured
                // Only for user-facing interactions: skip if either side is an MCP server or session memory node
                if (!session_memory_node_id_.empty() && source_id != session_memory_node_id_) {
                    bool source_is_mcp = false;
                    bool target_is_mcp = false;
                    for (const auto& node : config_.nodes) {
                        if (node.id == source_id) source_is_mcp = node.is_mcp_server || node.session_memory;
                        if (node.id == target)    target_is_mcp = node.is_mcp_server || node.session_memory;
                    }
                    if (!source_is_mcp && !target_is_mcp) {
                        auto req_it = request_payloads_.find(global_id_str);
                        if (req_it != request_payloads_.end()) {
                            MessageFrame memory_frame;
                            memory_frame.source_id = "luotsi-hub";
                            memory_frame.target_id = session_memory_node_id_;
                            memory_frame.payload = {
                                {"jsonrpc", "2.0"},
                                {"method", "luotsi/interaction"},
                                {"params", {
                                    {"source", target},
                                    {"target", source_id},
                                    {"prompt", req_it->second},
                                    {"completion", frame.payload}
                                }}
                            };
                            if (ports_.count(session_memory_node_id_)) {
                                spdlog::info("Forking interaction to session memory: {} -> {}", target, source_id);
                                ports_[session_memory_node_id_]->send(memory_frame);
                            } else {
                                spdlog::warn("Session memory node '{}' not found in ports", session_memory_node_id_);
                            }
                            request_payloads_.erase(req_it);
                        }
                    }
                }
                
                frame.target_id = target;
                target_port_it->second->send(frame);
                pending_requests_.erase(it);
                return;
            }
        }

        // Check for aggregated request/response tracking
        auto agg_it = pending_aggregations_.find(global_id_str);
        if (agg_it != pending_aggregations_.end()) {
            PendingAggregation& agg = agg_it->second;
            
            // Validate source_id matches one of the expected targets
            if (agg.pending_targets.count(source_id)) {
                spdlog::info("Received fan-out response from {} for aggregated request {}", source_id, global_id_str);
                
                nlohmann::json node_resp = frame.payload;
                node_resp["_source_id"] = source_id; 
                agg.responses.push_back(node_resp);
                agg.pending_targets.erase(source_id);

                if (agg.pending_targets.empty()) {
                    spdlog::info("All fan-out targets complete for {}, aggregating...", global_id_str);
                    
                    auto source_port_it = ports_.find(agg.source_id);
                    if (source_port_it != ports_.end()) {
                        nlohmann::json final_response;
                        final_response["jsonrpc"] = "2.0";
                        final_response["id"] = agg.original_id;
                        
                        // MCP Aggregation Logic
                        if (agg.method == "tools/list") {
                            nlohmann::json merged_tools = nlohmann::json::array();
                            for (const auto& resp : agg.responses) {
                                if (resp.contains("result") && resp["result"].contains("tools")) {
                                    std::string provider = resp["_source_id"];
                                    for (auto tool : resp["result"]["tools"]) {
                                        std::string original_name = tool["name"];
                                        tool["name"] = provider + "__" + original_name;
                                        merged_tools.push_back(tool);
                                    }
                                }
                            }
                            final_response["result"] = { {"tools", merged_tools} };
                        } else if (agg.method == "resources/list") {
                            nlohmann::json merged_resources = nlohmann::json::array();
                            for (const auto& resp : agg.responses) {
                                if (resp.contains("result") && resp["result"].contains("resources")) {
                                    for (const auto& res : resp["result"]["resources"]) {
                                        merged_resources.push_back(res);
                                    }
                                }
                            }
                            final_response["result"] = { {"resources", merged_resources} };
                        } else if (agg.method == "initialize") {
                            for (const auto& resp : agg.responses) {
                                if (resp.contains("result")) {
                                    final_response["result"] = resp["result"];
                                    break; 
                                }
                            }
                        } else {
                            for (const auto& resp : agg.responses) {
                                if (resp.contains("result")) {
                                    final_response["result"] = resp["result"];
                                    break;
                                }
                            }
                        }

                        MessageFrame final_frame;
                        final_frame.source_id = "luotsi-hub";
                        final_frame.target_id = agg.source_id;
                        final_frame.payload = final_response;

                        // Fork to session memory if configured
                        // Only for user-facing interactions: skip if the requester is an MCP server or session memory node
                        if (!session_memory_node_id_.empty() && agg.source_id != session_memory_node_id_) {
                            bool source_is_mcp = false;
                            for (const auto& node : config_.nodes) {
                                if (node.id == agg.source_id) {
                                    source_is_mcp = node.is_mcp_server || node.session_memory;
                                    break;
                                }
                            }
                            if (!source_is_mcp) {
                                MessageFrame memory_frame;
                                memory_frame.source_id = "luotsi-hub";
                                memory_frame.target_id = session_memory_node_id_;
                                memory_frame.payload = {
                                    {"jsonrpc", "2.0"},
                                    {"method", "luotsi/interaction"},
                                    {"params", {
                                        {"source", agg.source_id},
                                        {"target", "luotsi-aggregator"},
                                        {"prompt", agg.original_request},
                                        {"completion", final_response}
                                    }}
                                };
                                if (ports_.count(session_memory_node_id_)) {
                                    spdlog::info("Forking aggregated interaction to session memory: {} -> luotsi-aggregator", agg.source_id);
                                    ports_[session_memory_node_id_]->send(memory_frame);
                                } else {
                                    spdlog::warn("Session memory node '{}' not found in ports", session_memory_node_id_);
                                }
                            }
                        }
                        
                        source_port_it->second->send(final_frame);
                    }
                    
                    pending_aggregations_.erase(agg_it);
                }
                return;
            }
        }
    }

    // 2. Request Routing
    if (frame.payload.contains("method")) {
        // method already assigned at the top
        
        // Handle Authentication
        if (method == "luotsi/authenticate") {
            std::string agent_key = frame.payload["params"]["secret_key"].get<std::string>();
            bool authenticated = false;
            std::string assigned_role = "";
            
            auto agent_port = std::dynamic_pointer_cast<ports::AgentPort>(ports_[source_id]);

            for (const auto& role : roles_) {
                if (role.secret_key == agent_key) {
                    assigned_role = role.name;
                    authenticated = true;
                    if (agent_port) agent_port->setRole(assigned_role);
                    spdlog::info("Agent '{}' authenticated as role '{}'", source_id, assigned_role);
                    break;
                }
            }
            
            MessageFrame auth_reply;
            auth_reply.source_id = "luotsi-hub";
            auth_reply.target_id = source_id;
            
            if (authenticated) {
                auth_reply.payload = { {"jsonrpc", "2.0"}, {"id", frame.payload["id"]}, {"result", {{"authenticated", true}, {"role", assigned_role}}} };
            } else {
                spdlog::warn("Agent '{}' failed authentication with key: {}", source_id, agent_key);
                auth_reply.payload = { {"jsonrpc", "2.0"}, {"id", frame.payload["id"]}, {"error", {{"code", -32000}, {"message", "Authentication Failed"}}} };
            }

            if (ports_.count(source_id)) {
                ports_[source_id]->send(auth_reply);
            }
            return;
        }

        if (frame.payload.contains("id")) {
             // NAT the ID (Create Global ID)
             nlohmann::json orig_id_val = frame.payload["id"];
             std::string global_id_val = orig_id_val.is_string() ? orig_id_val.get<std::string>() : orig_id_val.dump();
             std::string global_id = source_id + ":" + global_id_val;
             
             pending_requests_[global_id] = source_id;
             original_ids_[global_id] = orig_id_val;
             request_payloads_[global_id] = frame.payload;
             
             frame.payload["id"] = global_id;
        }
    }

    for (const auto& route : source_config->routes) {
        if (!method.empty() && (route.trigger == "*" || method.find(route.trigger) == 0)) {
            
            if (route.action == "fan_out_mcp" && frame.payload.contains("id")) {
                std::string base_global_id = frame.payload["id"].is_string() ? frame.payload["id"].get<std::string>() : frame.payload["id"].dump();
                spdlog::info("Fanning out request {} from {}", method, source_id);

                PendingAggregation agg;
                agg.source_id = source_id;
                agg.original_id = original_ids_[base_global_id]; 
                agg.method = method;
                agg.original_request = frame.payload;

                for (const auto& target : route.targets) {
                    if (ports_.count(target)) {
                        agg.pending_targets.insert(target);
                    } else {
                        spdlog::error("Fan-out target '{}' not found in ports", target);
                    }
                }

                if (!agg.pending_targets.empty()) {
                    pending_aggregations_[base_global_id] = agg;
                    pending_requests_.erase(base_global_id);
                    original_ids_.erase(base_global_id);

                    for (const auto& target : agg.pending_targets) {
                        MessageFrame target_frame = frame;
                        target_frame.target_id = target;
                        target_frame.payload["id"] = base_global_id; 
                        ports_[target]->send(target_frame);
                    }
                } else {
                     spdlog::warn("Fan-out failed: No valid ports for {}", method);
                }
                return;
            } else if (route.action == "mcp_call_router" && method == "tools/call") {
                if (frame.payload.contains("params") && frame.payload["params"].contains("name")) {
                    std::string full_name = frame.payload["params"]["name"].get<std::string>();
                    size_t pos = full_name.find("__");
                    if (pos != std::string::npos) {
                        std::string target_provider = full_name.substr(0, pos);
                        std::string actual_tool = full_name.substr(pos + 2);
                        
                        auto target_port_it = ports_.find(target_provider);
                        if (target_port_it != ports_.end()) {
                            spdlog::info("Call Router extracted provider {}, tool: {}", target_provider, actual_tool);
                            frame.payload["params"]["name"] = actual_tool;
                            frame.target_id = target_provider;

                            std::string global_id_str = frame.payload["id"].is_string() ? frame.payload["id"].get<std::string>() : frame.payload["id"].dump();
                            pending_requests_[global_id_str] = source_id; 

                            target_port_it->second->send(frame);
                            return;
                        } else {
                             spdlog::error("Call Router failed: Target port '{}' not found", target_provider);
                        }
                    } 
                }
            } else if (route.action == "mcp_registry_query") {
                spdlog::info("Serving {} from virtual registry for {}", method, source_id);
                nlohmann::json final_response;
                final_response["jsonrpc"] = "2.0";
                
                std::string global_id_str = frame.payload["id"].is_string() ? frame.payload["id"].get<std::string>() : frame.payload["id"].dump();
                auto orig_it = original_ids_.find(global_id_str);
                final_response["id"] = (orig_it != original_ids_.end()) ? orig_it->second : frame.payload["id"];

                // ------------------ POLICY ENFORCEMENT ------------------
                std::string active_role_name = get_active_role_name(frame, source_id);
                std::vector<std::string> allowed_servers;

                for (const auto& role : roles_) {
                    if (role.name == active_role_name) {
                        allowed_servers = role.allowed_servers;
                        break;
                    }
                }

                auto is_server_allowed = [&](const std::string& target) {
                    for (const auto& a : allowed_servers) {
                        if (a == "*" || a == target) return true;
                    }
                    return false;
                };
                
                auto get_permitted_mcp_ports = [&]() {
                    std::vector<std::shared_ptr<ports::McpPort>> results;
                    for (const auto& [id, port] : ports_) {
                        if (auto mcp = std::dynamic_pointer_cast<ports::McpPort>(port)) {
                            if (is_server_allowed(id)) results.push_back(mcp);
                        }
                    }
                    return results;
                };
                
                auto permitted_mcp_ports = get_permitted_mcp_ports();
                // --------------------------------------------------------

                if (method == "tools/list") {
                    nlohmann::json filtered = nlohmann::json::array();
                    for (const auto& p : permitted_mcp_ports) {
                        for (auto item : p->getCapabilities("tools")) { 
                            std::string tool_name = item["name"];
                            // Tool name in cache is already namespaced: "provider__tool"
                            // Translate to policy format: "provider:tool"
                            std::string policy_name = tool_name;
                            size_t pos = policy_name.find("__");
                            if (pos != std::string::npos) {
                                policy_name.replace(pos, 2, ":");
                            }

                            if (is_tool_allowed(active_role_name, policy_name)) {
                                filtered.push_back(item); 
                            }
                        }
                    }
                    final_response["result"] = {{"tools", filtered}};
                } else if (method == "resources/list") {
                    nlohmann::json filtered = nlohmann::json::array();
                    for (const auto& p : permitted_mcp_ports) {
                        for (const auto& item : p->getCapabilities("resources")) { 
                            if (is_resource_allowed(active_role_name, item["uri"])) {
                                filtered.push_back(item); 
                            }
                        }
                    }
                    final_response["result"] = {{"resources", filtered}};
                } else if (method == "resources/templates/list") {
                    nlohmann::json filtered = nlohmann::json::array();
                    for (const auto& p : permitted_mcp_ports) {
                        for (const auto& item : p->getCapabilities("templates")) { 
                            if (is_resource_allowed(active_role_name, item["uriTemplate"])) {
                                filtered.push_back(item); 
                            }
                        }
                    }
                    final_response["result"] = {{"resourceTemplates", filtered}};
                } else if (method == "prompts/list") {
                    nlohmann::json merged = nlohmann::json::array();
                    for (const auto& p : permitted_mcp_ports) {
                        for (const auto& item : p->getCapabilities("prompts")) { merged.push_back(item); }
                    }
                    final_response["result"] = {{"prompts", merged}};
                } else if (method == "initialize") {
                    final_response["result"] = {
                        {"protocolVersion", "2024-11-05"},
                        {"capabilities", nlohmann::json::object()},
                        {"serverInfo", {{"name", "luotsi-hub"}, {"version", "1.0.0"}}}
                    };
                }

                MessageFrame reply_frame;
                reply_frame.source_id = "luotsi-hub";
                reply_frame.target_id = source_id;
                reply_frame.payload = final_response;
                
                if (ports_.count(source_id)) {
                    ports_[source_id]->send(reply_frame);
                    pending_requests_.erase(global_id_str);
                    original_ids_.erase(global_id_str);
                }
                return;
            } else {
                // Normal uni-directonal routing
                auto target_port_it = ports_.find(route.target);
                if (target_port_it != ports_.end()) {
                    spdlog::info("Routing Request {} -> {}", source_id, route.target);
                    
                    if (route.action == "translate" && !route.new_method.empty()) {
                        spdlog::info("Translating method '{}' -> '{}'", method, route.new_method);
                        frame.payload["method"] = route.new_method;
                    }
                    
                    frame.target_id = route.target;

                    if (route.action == "fan_out_mcp" && !frame.payload.contains("id")) {
                        for (const auto& tgt : route.targets) {
                            if (ports_.count(tgt)) ports_[tgt]->send(frame);
                        }
                        return;
                    }

                    target_port_it->second->send(frame);
                } else {
                    spdlog::error("Route target '{}' not found in ports", route.target);
                }
                return;
            }
        }
    }
    
    // Fallback: route to master node if configured, unless source IS the master (anti-loop)
    if (!master_node_id_.empty()) {
        if (source_id == master_node_id_) {
            spdlog::warn("No route found for message from master node '{}' (method: '{}'). Dropping to prevent loop.", source_id, method);
        } else if (ports_.count(master_node_id_)) {
            bool source_is_agent = false;
            for (const auto& node : config_.nodes) {
                if (node.id == source_id) {
                    source_is_agent = node.is_mcp_server;
                    break;
                }
            }
            if (source_is_agent) {
                spdlog::info("No route matched for '{}' from agent '{}'. Forwarding to master node '{}'.", method, source_id, master_node_id_);
                frame.target_id = master_node_id_;
                frame.payload["_routed_to_master"] = true;
                ports_[master_node_id_]->send(frame);
            } else {
                spdlog::warn("No route found for message from gateway '{}' (method: '{}'). Dropping.", source_id, method);
            }
        }
    } else {
        spdlog::warn("No route found for message from {} (method: {})", source_id, method);
    }
}

void Runtime::sanitize_payload(nlohmann::json& payload, size_t limit) {
    if (payload.is_string()) {
        std::string s = payload.get<std::string>();
        if (s.size() > limit) {
            spdlog::warn("Field exceeds max_token_size ({} > {}). Stripping.", s.size(), limit);
            payload = "[BLOCKED: Field exceeds max_token_size]";
        }
    } else if (payload.is_object()) {
        for (auto it = payload.begin(); it != payload.end(); ++it) {
            sanitize_payload(it.value(), limit);
        }
    } else if (payload.is_array()) {
        for (auto& item : payload) {
            sanitize_payload(item, limit);
        }
    }
}

std::string Runtime::get_active_role_name(const MessageFrame& frame, const std::string& source_id) {
    auto agent_port = std::dynamic_pointer_cast<ports::AgentPort>(ports_[source_id]);
    if (!agent_port || !agent_port->isAuthenticated()) return "";

    std::string base_role = agent_port->getRole();
    bool is_source_trusted = false;
    for (const auto& role : roles_) {
        if (role.name == base_role) {
            is_source_trusted = role.is_trusted;
            break;
        }
    }

    if (!frame.delegated_role.empty() && is_source_trusted) {
        return frame.delegated_role;
    }
    return base_role;
}

bool Runtime::is_tool_allowed(const std::string& role_name, const std::string& tool_name) {
    if (role_name.empty()) return false;
    
    const PolicyRole* active_role = nullptr;
    for (const auto& role : roles_) {
        if (role.name == role_name) {
            active_role = &role;
            break;
        }
    }

    if (!active_role) return false;

    // 1. Check Blocklist (Deny-Wins)
    for (const auto& pattern : active_role->blocked_tools) {
        if (wildcard_match(pattern, tool_name)) return false;
    }

    // 2. Check Allowlist
    if (active_role->allowed_tools.empty()) return true; // Default allow if no tool restrictions defined for role?
    // Actually, if we want to be secure by default, we should check if any allowed_tools are defined.
    // But for backward compatibility with "allowed_servers" only, let's say if allowed_tools is empty, it's allowed.
    // Wait, let's look at the requirement: "right now we dont have any restriction for agents using mcp server full potential"
    // So if allowed_tools is empty, it probably means "full potential" (if server is allowed).
    
    for (const auto& pattern : active_role->allowed_tools) {
        if (wildcard_match(pattern, tool_name)) return true;
    }

    return false;
}

bool Runtime::is_resource_allowed(const std::string& role_name, const std::string& resource_uri) {
    if (role_name.empty()) return false;
    
    const PolicyRole* active_role = nullptr;
    for (const auto& role : roles_) {
        if (role.name == role_name) {
            active_role = &role;
            break;
        }
    }

    if (!active_role) return false;

    for (const auto& pattern : active_role->blocked_resources) {
        if (wildcard_match(pattern, resource_uri)) return false;
    }

    if (active_role->allowed_resources.empty()) return true;
    
    for (const auto& pattern : active_role->allowed_resources) {
        if (wildcard_match(pattern, resource_uri)) return true;
    }

    return false;
}

} // namespace luotsi::internal

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
    if (new_config.audit_log || new_config.observability_endpoint) {
        if (!observability_) {
            std::string path = new_config.audit_log ? *new_config.audit_log : "";
            std::string endpoint = new_config.observability_endpoint ? *new_config.observability_endpoint : "";
            observability_ = std::make_unique<Observability>(path, endpoint);
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

    adapter->init(node_cfg, roles_);
    
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
        dispatch(node_cfg.id, init_frame);
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

// GC functions removed.

void Runtime::dispatch(const std::string& target_id, luotsi::MessageFrame& frame) {
    auto target_port_it = ports_.find(target_id);
    if (target_port_it != ports_.end()) {
        frame.target_id = target_id;
        
        if (!frame.trace_id.empty() && !frame.span_id.empty()) {
            std::string traceparent = "00-" + frame.trace_id + "-" + frame.span_id + "-01";
            if (frame.payload.contains("params") && frame.payload["params"].is_object()) {
                frame.payload["params"]["_meta"]["traceparent"] = traceparent;
            } else {
                frame.payload["_meta"]["traceparent"] = traceparent;
            }
        }

        // Emit observability event here: both source_id and target_id are now known.
        if (observability_) {
            observability_->log_message(frame);
        }

        target_port_it->second->send(frame);

        // Smart Fabric: Update NAT entry with the resolved target_id for future causal inference.
        // For outbound requests, frame.payload["id"] already contains the global_id.
        if (frame.payload.contains("id") && frame.payload.contains("method")) {
            std::string gid = frame.payload["id"].is_string() ? 
                              frame.payload["id"].get<std::string>() : 
                              frame.payload["id"].dump();
            auto it = nat_table_.find(gid);
            if (it != nat_table_.end()) {
                it->second.target_id = target_id;
            }
        }
    } else {
        spdlog::warn("Dispatch failed: target '{}' not found for message from '{}'", target_id, frame.source_id);
    }
}

void Runtime::route_message(luotsi::MessageFrame& frame, const std::string& source_id) {
    // --- PROTOCOL ENFORCEMENT ---
    // Basic validation: Ensure it's a valid JSON-RPC 2.0 object.
    // Legacy role/user delegation fields are no longer supported or checked explicitly.
    if (!frame.payload.is_object() || !frame.payload.contains("jsonrpc")) {
        spdlog::warn("Protocol violation: node '{}' sent malformed payload. Dropping message.", source_id);
        return;
    }

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

    spdlog::info("Bus received from {}: {}", source_id, frame.payload.dump());

    // 1. Session Context Propagation (Smart Fabric Inference)
    if (frame.trace_id.empty() && frame.payload.contains("id") && frame.payload.contains("method")) {
        NatEntry* parent = find_most_likely_parent(source_id);
        if (parent) {
            // Sub-request from node: transparently inherit the root session trace.
            frame.trace_id       = parent->trace_id;
            frame.parent_span_id = parent->span_id;
            frame.span_id        = generate_span_id();
            spdlog::info("Smart Fabric: Inferred parent trace {} for sub-request from {} (parent_span={})",
                         frame.trace_id, source_id, frame.parent_span_id);
        } else {
            // Root request (external caller) → try to inherit from persistent session cache
            std::string user_id;
            if (frame.payload.contains("params")) {
                if (frame.payload["params"].contains("from") && frame.payload["params"]["from"].is_string()) {
                    user_id = frame.payload["params"]["from"].get<std::string>();
                } else if (frame.payload["params"].contains("session_id") && frame.payload["params"]["session_id"].is_string()) {
                    user_id = frame.payload["params"]["session_id"].get<std::string>();
                }
            }

            auto s_it = session_trace_cache_.find(source_id + "::" + user_id);
            if (!user_id.empty() && s_it != session_trace_cache_.end()) {
                frame.trace_id       = s_it->second.trace_id;
                // For a new user message, the previous final response span is the parent
                frame.parent_span_id = s_it->second.span_id;
                frame.span_id        = generate_span_id();
                s_it->second.last_seen = std::chrono::steady_clock::now();
                spdlog::info("Continued user session trace {} for {} (+{})", 
                             frame.trace_id, source_id, user_id);
            } else {
                frame.trace_id = generate_trace_id();
                frame.span_id  = generate_span_id();
                spdlog::info("New root trace {} from {} span={}", frame.trace_id, source_id, frame.span_id);
            }
        }
    }

    // NOTE: observability log_message is called inside dispatch() once target_id is resolved.

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
                 dispatch(source_id, error_reply);
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
                 dispatch(source_id, error_reply);
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
            
            // Emit CloudEvent for this discovery response. These messages never reach dispatch(),
            // so we must log here to ensure the observability UDP stream captures capability data.
            if (observability_) {
                observability_->log_message(frame);
            }

            auto mcp_port = std::dynamic_pointer_cast<ports::McpPort>(ports_[source_id]);
            if (!mcp_port) return;

            if (global_id_str == "__luotsi__init__" + source_id) {
                // Initialize a trace for the discovery process
                std::string discovery_trace = generate_trace_id();
                std::string discovery_span = generate_span_id();

                MessageFrame notif;
                notif.source_id = "luotsi-hub";
                notif.target_id = source_id;
                notif.trace_id = discovery_trace;
                notif.span_id = discovery_span;
                notif.payload = { {"jsonrpc", "2.0"}, {"method", "notifications/initialized"} };
                dispatch(source_id, notif);
                
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
                    req.trace_id = discovery_trace;
                    req.span_id = generate_span_id();
                    req.payload = { {"jsonrpc", "2.0"}, {"id", "__luotsi__tools__" + source_id}, {"method", "tools/list"} };
                    dispatch(source_id, req);
                }

                if (!is_disabled("resources/list")) {
                    MessageFrame req2;
                    req2.source_id = "luotsi-hub";
                    req2.target_id = source_id;
                    req2.trace_id = discovery_trace;
                    req2.span_id = generate_span_id();
                    req2.payload = { {"jsonrpc", "2.0"}, {"id", "__luotsi__resources__" + source_id}, {"method", "resources/list"} };
                    dispatch(source_id, req2);
                }
                
                if (!is_disabled("resources/templates/list")) {
                    MessageFrame req3;
                    req3.source_id = "luotsi-hub";
                    req3.target_id = source_id;
                    req3.trace_id = discovery_trace;
                    req3.span_id = generate_span_id();
                    req3.payload = { {"jsonrpc", "2.0"}, {"id", "__luotsi__templates__" + source_id}, {"method", "resources/templates/list"} };
                    dispatch(source_id, req3);
                }
                
                if (!is_disabled("prompts/list")) {
                    MessageFrame req4;
                    req4.source_id = "luotsi-hub";
                    req4.target_id = source_id;
                    req4.payload = { {"jsonrpc", "2.0"}, {"id", "__luotsi__prompts__" + source_id}, {"method", "prompts/list"} };
                    dispatch(source_id, req4);
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

        // Check for normal request/response tracking via NAT table
        auto it = nat_table_.find(global_id_str);
        if (it != nat_table_.end() && !it->second.fulfilled) {
            std::string target = it->second.source_id;
            // Validate target still exists
            auto target_port_it = ports_.find(target);
            if (target_port_it != ports_.end()) {
                spdlog::info("Auto-routing Response {} -> {}", source_id, target);
                
                // Un-NAT the ID (Restore original ID from NAT entry)
                frame.payload["id"] = it->second.original_id;

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
                        MessageFrame memory_frame;
                        memory_frame.source_id = "luotsi-hub";
                        memory_frame.target_id = session_memory_node_id_;
                        memory_frame.payload = {
                            {"jsonrpc", "2.0"},
                            {"method", "luotsi/interaction"},
                            {"params", {
                                {"source", target},
                                {"target", source_id},
                                {"prompt", it->second.request_payload},
                                {"completion", frame.payload}
                            }}
                        };
                        memory_frame.trace_id       = it->second.trace_id;
                        memory_frame.parent_span_id = it->second.span_id;
                        memory_frame.span_id        = generate_span_id();
                        if (ports_.count(session_memory_node_id_)) {
                            spdlog::info("Forking interaction to session memory: {} -> {}", target, source_id);
                            dispatch(session_memory_node_id_, memory_frame);
                        } else {
                            spdlog::warn("Session memory node '{}' not found in ports", session_memory_node_id_);
                        }
                    }
                }
                
                // Restore trace context from the original request so the response span
                // carries the same trace_id — completing the round-trip in one trace.
                // Response frames from MCP servers carry no traceparent themselves.
                if (frame.trace_id.empty()) {
                    frame.trace_id       = it->second.trace_id;
                    frame.parent_span_id = it->second.span_id;
                    frame.span_id        = generate_span_id();
                }

                // Build PendingRequestState for span emission from the NAT entry
                PendingRequestState req_state {
                    it->second.source_id,
                    it->second.trace_id,
                    it->second.span_id,
                    it->second.parent_span_id,
                    it->second.start_time
                };

                dispatch(target, frame);
                
                // Update session persistence cache for this user
                {
                    std::string user_id;
                    if (it->second.request_payload.contains("params")) {
                        if (it->second.request_payload["params"].contains("from") && it->second.request_payload["params"]["from"].is_string()) {
                            user_id = it->second.request_payload["params"]["from"].get<std::string>();
                        } else if (it->second.request_payload["params"].contains("session_id") && it->second.request_payload["params"]["session_id"].is_string()) {
                            user_id = it->second.request_payload["params"]["session_id"].get<std::string>();
                        }
                    }
                    if (!user_id.empty()) {
                        std::string session_key = target + "::" + user_id;
                        session_trace_cache_[session_key] = { frame.trace_id, frame.span_id, std::chrono::steady_clock::now() };
                    }
                }
                
                bool is_trace_end = false;
                if (observability_) {
                    auto duration = std::chrono::steady_clock::now() - it->second.start_time;
                    auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
                    
                    for (const auto& node : config_.nodes) {
                        if (node.id == target) {
                            if (node.is_gateway) is_trace_end = true;
                            break;
                        }
                    }
                    observability_->log_span(req_state, frame, duration_ms, is_trace_end);
                }

                if (is_trace_end) {
                    session_trace_cache_.clear();
                }

                // Immediately erase the fulfilled NAT entry
                auto idx = std::find(nat_insertion_order_.begin(), nat_insertion_order_.end(), global_id_str);
                if (idx != nat_insertion_order_.end()) nat_insertion_order_.erase(idx);
                nat_table_.erase(it);
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
                    
                    if (ports_.count(agg.source_id)) {
                        nlohmann::json final_response;
                        final_response["jsonrpc"] = "2.0";
                        final_response["id"] = agg.original_id;
                        
                        // MCP Aggregation Logic
                        bool has_error = false;
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
                                if (resp.contains("error")) has_error = true;
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
                                if (resp.contains("error")) has_error = true;
                            }
                            final_response["result"] = { {"resources", merged_resources} };
                        } else if (agg.method == "initialize") {
                            for (const auto& resp : agg.responses) {
                                if (resp.contains("result")) {
                                    final_response["result"] = resp["result"];
                                    break; 
                                }
                                if (resp.contains("error")) has_error = true;
                            }
                        } else {
                            for (const auto& resp : agg.responses) {
                                if (resp.contains("result")) {
                                    final_response["result"] = resp["result"];
                                    break;
                                }
                                if (resp.contains("error")) has_error = true;
                            }
                        }

                        // Build the consolidated reply frame — carry trace context through
                        MessageFrame final_frame;
                        final_frame.source_id    = "luotsi-hub";
                        final_frame.target_id    = agg.source_id;
                        final_frame.payload      = final_response;
                        final_frame.trace_id     = agg.trace_id;
                        final_frame.parent_span_id = agg.span_id;
                        final_frame.span_id      = generate_span_id();

                        // Fork to session memory if configured
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
                                memory_frame.trace_id       = agg.trace_id;
                                memory_frame.parent_span_id = agg.span_id;
                                memory_frame.span_id        = generate_uuid_v4().substr(0, 16);
                                if (ports_.count(session_memory_node_id_)) {
                                    spdlog::info("Forking aggregated interaction to session memory: {} -> luotsi-aggregator", agg.source_id);
                                    dispatch(session_memory_node_id_, memory_frame);
                                } else {
                                    spdlog::warn("Session memory node '{}' not found in ports", session_memory_node_id_);
                                }
                            }
                        }

                        // Use dispatch() so traceparent is injected and log_message fires —
                        // making the aggregated reply visible in the observability stream.
                        dispatch(agg.source_id, final_frame);

                        // Emit a fan-out span so the dashboard can show total aggregation latency.
                        if (observability_) {
                            auto duration = std::chrono::steady_clock::now() - agg.start_time;
                            auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
                            observability_->log_aggregation_span(
                                agg.trace_id, agg.span_id, agg.source_id,
                                agg.method, agg.responses.size(), duration_ms, has_error);
                        }
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
        
        // Handle Authentication (Strictly for TCP/Remote nodes)
        if (method == "luotsi/authenticate") {
            bool is_tcp = (source_config && source_config->runtime.adapter == "jsonrpc_tcp");
            
            if (!is_tcp) {
                spdlog::warn("Access Denied: Node '{}' attempted authentication over non-TCP adapter. Stdio nodes are auto-authenticated.", source_id);
                if (frame.payload.contains("id")) {
                    MessageFrame error_reply;
                    error_reply.source_id = "luotsi-hub";
                    error_reply.target_id = source_id;
                    error_reply.payload = { {"jsonrpc", "2.0"}, {"id", frame.payload["id"]}, {"error", {{"code", -32001}, {"message", "Access Denied: luotsi/authenticate is reserved for TCP connections"}}} };
                    if (ports_.count(source_id)) ports_[source_id]->send(error_reply);
                }
                return;
            }

            std::string agent_key = frame.payload.contains("params") && frame.payload["params"].contains("secret_key") ? 
                                    frame.payload["params"]["secret_key"].get<std::string>() : "";
            
            bool authenticated = false;
            std::string assigned_role = "";
            
            auto agent_port = std::dynamic_pointer_cast<ports::AgentPort>(ports_[source_id]);

            for (const auto& role : roles_) {
                if (!role.secret_key.empty() && role.secret_key == agent_key) {
                    assigned_role = role.name;
                    authenticated = true;
                    break;
                }
            }

            if (authenticated && agent_port) {
                spdlog::info("TCP Agent '{}' session established as role '{}'", source_id, assigned_role);
            }
            
            MessageFrame auth_reply;
            auth_reply.source_id = "luotsi-hub";
            auth_reply.target_id = source_id;
            
            if (authenticated) {
                auth_reply.payload = { {"jsonrpc", "2.0"}, {"id", frame.payload["id"]}, {"result", {{"authenticated", true}, {"role", assigned_role}}} };
            } else {
                spdlog::warn("TCP Agent '{}' failed authentication with key: {}", source_id, agent_key);
                auth_reply.payload = { {"jsonrpc", "2.0"}, {"id", frame.payload["id"]}, {"error", {{"code", -32000}, {"message", "Authentication Failed"}}} };
            }

            if (ports_.count(source_id)) {
                ports_[source_id]->send(auth_reply);
            }
            return;
        }
        // --- NATIVE PROTOCOL ENGINE & RBAC ---
        if (!method.empty() && method != "luotsi/authenticate") {
            std::string active_role_name = get_active_role_name(frame, source_id);
            
            if (!roles_.empty() && !is_authorized(active_role_name, method)) {
                spdlog::warn("Access Denied: Role '{}' attempted unpermitted outgoing method '{}'", active_role_name, method);
                if (frame.payload.contains("id")) {
                    MessageFrame error_reply;
                    error_reply.source_id = "luotsi-hub";
                    error_reply.target_id = source_id;
                    error_reply.payload = { {"jsonrpc", "2.0"}, {"id", frame.payload["id"]}, {"error", {{"code", -32001}, {"message", "Access Denied: Unpermitted outgoing method"}}} };
                    if (ports_.count(source_id)) ports_[source_id]->send(error_reply);
                }
                return;
            }

            // Pre-evaluate routes to detect explicit overrides
            std::vector<RouteConfig> pre_eval_routes = source_config ? source_config->routes : std::vector<RouteConfig>();
            bool has_exact_route = false;
            for (const auto& route : pre_eval_routes) {
                 if (!route.trigger.empty() && route.trigger != "*" && method.find(route.trigger) == 0) { 
                     has_exact_route = true; 
                     break; 
                 }
            }

            if (!has_exact_route) {
                // Native Hooks
                if (method == "roots/list") {
                    spdlog::info("Serving roots internally for {}", source_id);
                    nlohmann::json roots_array = nlohmann::json::array();
                    if (source_config && !source_config->allowed_roots.empty()) {
                        for (const auto& path : source_config->allowed_roots) {
                            roots_array.push_back({
                                {"uri", "file://" + path},
                                {"name", path}
                            });
                        }
                    }
                    MessageFrame reply_frame;
                    reply_frame.source_id = "luotsi-hub";
                    reply_frame.target_id = source_id;
                    reply_frame.payload = {
                        {"jsonrpc", "2.0"},
                        {"id", frame.payload.contains("id") ? frame.payload["id"] : nullptr},
                        {"result", {{"roots", roots_array}}}
                    };
                    if (ports_.count(source_id)) ports_[source_id]->send(reply_frame);
                    return;
                }

                if (method.find("notifications/") == 0) {
                    if (method == "notifications/tools/list_changed" || 
                        method == "notifications/resources/list_changed" || 
                        method == "notifications/prompts/list_changed") {
                        
                        spdlog::info("Refreshing MCP capabilities triggered by {} from {}", method, source_id);
                        std::string target_method;
                        std::string target_id;
                        
                        if (method == "notifications/tools/list_changed") {
                            target_method = "tools/list";
                            target_id = "__luotsi__tools__" + source_id;
                        } else if (method == "notifications/resources/list_changed") {
                            target_method = "resources/list";
                            target_id = "__luotsi__resources__" + source_id;
                        } else {
                            target_method = "prompts/list";
                            target_id = "__luotsi__prompts__" + source_id;
                        }

                        auto is_disabled = [&](const std::string& cap) {
                            if (source_config) {
                                for (const auto& d : source_config->disabled_capabilities) {
                                    if (d == cap) return true;
                                }
                            }
                            return false;
                        };

                        if (!is_disabled(target_method)) {
                            MessageFrame req;
                            req.source_id = "luotsi-hub";
                            req.target_id = source_id;
                            req.payload = { {"jsonrpc", "2.0"}, {"id", target_id}, {"method", target_method} };
                            if (ports_.count(source_id)) ports_[source_id]->send(req);
                        }
                        return;
                    }
                    
                    // Check if routed by wildcard. If not, absorb natively.
                    bool explicitly_routed = false;
                    for (const auto& route : pre_eval_routes) {
                         if (route.trigger == "*" || method.find(route.trigger) == 0) { explicitly_routed = true; break; }
                    }
                    if (!explicitly_routed) {
                        spdlog::debug("Absorbing standard notification natively: {} from {}", method, source_id);
                        return;
                    }
                }
            }
        }

        if (frame.payload.contains("id")) {
             // NAT the ID (Create Global ID)
             nlohmann::json orig_id_val = frame.payload["id"];
             std::string global_id_val = orig_id_val.is_string() ? orig_id_val.get<std::string>() : orig_id_val.dump();
             std::string global_id = source_id + ":" + global_id_val;

             // Consolidated NAT insert — replaces the old 3-map approach.
             nat_table_[global_id] = {
                 source_id,
                 "",                // target_id will be populated during dispatch
                 orig_id_val,
                 frame.payload,          // request_payload for session memory
                 frame.trace_id,
                 frame.span_id,
                 frame.parent_span_id,
                 frame.timestamp,
                 /*fulfilled=*/false
             };
             nat_insertion_order_.push_back(global_id);
             frame.payload["id"] = global_id;
        }
    }

    std::vector<RouteConfig> routes_to_evaluate = source_config->routes;
    
    // --- NATIVE PROTOCOL ENGINE & RBAC --- (Moved up)
    
    // Implicit Agent Routes
    if (source_config->is_agent) {
        routes_to_evaluate.push_back({"initialize", "", {}, "mcp_registry_query", ""});
        routes_to_evaluate.push_back({"tools/list", "", {}, "mcp_registry_query", ""});
        routes_to_evaluate.push_back({"resources/list", "", {}, "mcp_registry_query", ""});
        routes_to_evaluate.push_back({"resources/templates/list", "", {}, "mcp_registry_query", ""});
        routes_to_evaluate.push_back({"prompts/list", "", {}, "mcp_registry_query", ""});
        routes_to_evaluate.push_back({"tools/call", "", {}, "mcp_call_router", ""});
        routes_to_evaluate.push_back({"notifications/initialized", "", {}, "fan_out_mcp", ""});
        routes_to_evaluate.push_back({"resources/read", "", {}, "mcp_resource_router", ""});
    }

    for (const auto& route : routes_to_evaluate) {
        if (!method.empty() && (route.trigger == "*" || method.find(route.trigger) == 0)) {
            
            if (route.action == "fan_out_mcp" && frame.payload.contains("id")) {
                std::string base_global_id = frame.payload["id"].is_string() ? frame.payload["id"].get<std::string>() : frame.payload["id"].dump();
                spdlog::info("Fanning out request {} from {}", method, source_id);

                // Retrieve trace context and original_id from the consolidated NAT table
                std::string fan_trace_id, fan_span_id, fan_parent_span_id;
                nlohmann::json fan_original_id = base_global_id;
                nlohmann::json fan_original_request = frame.payload;
                auto nat_it = nat_table_.find(base_global_id);
                if (nat_it != nat_table_.end()) {
                    fan_trace_id         = nat_it->second.trace_id;
                    fan_span_id          = nat_it->second.span_id;
                    fan_parent_span_id   = nat_it->second.parent_span_id;
                    fan_original_id      = nat_it->second.original_id;
                    fan_original_request = nat_it->second.request_payload;
                    // Mark fulfilled so GC can reclaim it — aggregation table owns routing now
                    nat_it->second.fulfilled = true;
                }

                PendingAggregation agg;
                agg.source_id       = source_id;
                agg.original_id     = fan_original_id;
                agg.method          = method;
                agg.original_request = fan_original_request;
                agg.trace_id        = fan_trace_id;
                agg.span_id         = fan_span_id;
                agg.start_time      = frame.timestamp;

                std::vector<std::string> active_targets = route.targets;
                
                // Auto discovery of targets based on RBAC policy if none explicit
                if (active_targets.empty()) {
                    std::string active_role_name = get_active_role_name(frame, source_id);
                    std::vector<std::string> allowed_servers;
                    for (const auto& role : roles_) {
                        if (role.name == active_role_name) {
                            allowed_servers = role.allowed_servers;
                            break;
                        }
                    }
                    for (const auto& [id, port] : ports_) {
                        if (std::dynamic_pointer_cast<ports::McpPort>(port)) {
                            bool allowed = false;
                            for (const auto& a : allowed_servers) {
                                if (a == "*" || a == id) { allowed = true; break; }
                            }
                            if (allowed) active_targets.push_back(id);
                        }
                    }
                }

                for (const auto& target : active_targets) {
                    if (ports_.count(target)) {
                        agg.pending_targets.insert(target);
                    } else {
                        spdlog::error("Fan-out target '{}' not found in ports", target);
                    }
                }

                if (!agg.pending_targets.empty()) {
                    pending_aggregations_[base_global_id] = agg;

                    // Dispatch each fan-out leg via dispatch() so traceparent is injected
                    // and every outbound hop appears in the observability stream.
                    for (const auto& target : agg.pending_targets) {
                        MessageFrame target_frame = frame;
                        target_frame.source_id = "luotsi-hub";
                        target_frame.target_id = target;
                        target_frame.payload["id"] = base_global_id;
                        // Each leg is a child span of the fan-out trace
                        target_frame.trace_id       = fan_trace_id;
                        target_frame.parent_span_id = fan_span_id;
                        target_frame.span_id        = generate_uuid_v4().substr(0, 16);
                        dispatch(target, target_frame);
                    }
                } else {
                     spdlog::warn("Fan-out failed: No valid ports for {}", method);
                }
                return;
            } else if (route.action == "mcp_resource_router" && method == "resources/read") {
                if (frame.payload.contains("params") && frame.payload["params"].contains("uri")) {
                    std::string uri = frame.payload["params"]["uri"].get<std::string>();
                    size_t pos = uri.find("://");
                    if (pos != std::string::npos) {
                        // Extract scheme and assume <scheme>_mcp
                        std::string target_provider = uri.substr(0, pos) + "_mcp";
                        
                        auto target_port_it = ports_.find(target_provider);
                        if (target_port_it != ports_.end()) {
                            spdlog::info("Resource Router extracted provider {} from URI: {}", target_provider, uri);
                            frame.target_id = target_provider;
                            // NAT table already has this entry from the earlier insert — no re-insert needed.
                            dispatch(target_provider, frame);
                            return;
                        } else {
                             spdlog::error("Resource Router failed: Target port '{}' not found for URI {}", target_provider, uri);
                        }
                    } 
                }
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
                            // NAT table already has this entry from the earlier insert — no re-insert needed.
                            dispatch(target_provider, frame);
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
                auto nat_it = nat_table_.find(global_id_str);
                final_response["id"] = (nat_it != nat_table_.end()) ? nat_it->second.original_id : frame.payload["id"];

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
                reply_frame.payload   = final_response;

                // Carry trace context through the virtual-registry reply so this
                // internal round-trip appears as a connected span in the dashboard.
                if (nat_it != nat_table_.end()) {
                    reply_frame.trace_id       = nat_it->second.trace_id;
                    reply_frame.parent_span_id = nat_it->second.span_id;
                    reply_frame.span_id        = generate_uuid_v4().substr(0, 16);

                    if (observability_) {
                        PendingRequestState req_state {
                            nat_it->second.source_id,
                            nat_it->second.trace_id,
                            nat_it->second.span_id,
                            nat_it->second.parent_span_id,
                            nat_it->second.start_time
                        };
                        auto dur = std::chrono::steady_clock::now() - nat_it->second.start_time;
                        auto dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(dur).count();
                        observability_->log_span(req_state, reply_frame, dur_ms);
                    }
                    nat_it->second.fulfilled = true;
                }

                if (ports_.count(source_id)) {
                    // Use dispatch() so traceparent header is injected and log_message fires.
                    dispatch(source_id, reply_frame);
                }
                return;
            } else {
                // Normal uni-directonal routing
                auto target_port_it = ports_.find(route.target);
                if (target_port_it != ports_.end()) {
                    spdlog::info("Routing Request {} -> {}", source_id, route.target);
                    
                    if (!route.new_method.empty()) {
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

                    // Use dispatch() so traceparent is injected, observability logged,
                    // and agent_active_trace_ is registered for session trace propagation.
                    dispatch(route.target, frame);
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
            payload = "[VALUE_OMITTED_DUE_TO_SIZE: " + std::to_string(s.size()) + " bytes]";
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
    std::string base_role = "";
    
    // First, check if the node has a static role in config
    for (const auto& node : config_.nodes) {
        if (node.id == source_id) {
            if (!node.role.empty()) {
                base_role = node.role;
            } else if (node.runtime.adapter == "stdio") {
                // Auto-assign roles for managed stdio nodes if none specified
                if (node.is_agent) base_role = "agent";
                else if (node.is_mcp_server) base_role = "mcp_server";
                else base_role = "guest";
                spdlog::debug("Auto-assigned role '{}' to managed stdio node '{}'", base_role, source_id);
            }
            break;
        }
    }
    
    // Fallback to runtime authentication context if no static role found
    if (base_role.empty()) {
        auto agent_port = std::dynamic_pointer_cast<ports::AgentPort>(ports_[source_id]);
        if (agent_port && agent_port->isAuthenticated()) {
            base_role = agent_port->getRole();
        }
    }
    
    if (base_role.empty()) {
        base_role = "guest"; // Lowest privilege fallback
    }

    bool is_source_trusted = false;
    for (const auto& role : roles_) {
        if (role.name == base_role) {
            is_source_trusted = role.is_trusted;
            break;
        }
    }

    // Dynamic payload role injection and delegation removed.
    // We strictly use the base_role determined by static configuration or port authentication.

    return base_role;
}

bool Runtime::is_authorized(const std::string& role_name, const std::string& method) {
    if (role_name.empty()) return false;
    
    const PolicyRole* active_role = nullptr;
    for (const auto& role : roles_) {
        if (role.name == role_name) {
            active_role = &role;
            break;
        }
    }
    if (!active_role) return false;
    
    if (active_role->allowed_methods.empty()) {
        return true; // Default allow for backward compatibility
    }
    
    for (const auto& m : active_role->allowed_methods) {
        if (wildcard_match(m, method)) return true;
    }
    return false;
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

luotsi::NatEntry* Runtime::find_most_likely_parent(const std::string& node_id) {
    // Search NAT table for the most recent unfulfilled request targeting this node.
    // We iterate backwards from most recent insertions for efficiency and causal alignment.
    for (auto it = nat_insertion_order_.rbegin(); it != nat_insertion_order_.rend(); ++it) {
        auto n_it = nat_table_.find(*it);
        if (n_it != nat_table_.end()) {
            const auto& entry = n_it->second;
            // The "node_id" is the suspected agent/node currently issuing a sub-request.
            // A causal parent is an inbound request where this node was the TARGET.
            // We only consider unfulfilled requests (active "causal shadows").
            if (!entry.fulfilled && entry.target_id == node_id) {
                return &n_it->second;
            }
        }
    }
    return nullptr;
}

} // namespace luotsi::internal

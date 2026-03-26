#pragma once

#include <string>
#include <map>
#include <memory>
#include <asio.hpp>
#include <set>
#include <vector>
#include <nlohmann/json.hpp>
#include "config.hpp"
#include "../ports/port_impl.hpp"

#include "internal_types.hpp"

#include "observability.hpp"

namespace luotsi::internal {

class Runtime {
public:
    Runtime(const std::string& config_path);
    void start();
    void stop(); 

private:
    void dispatch(const std::string& target_id, luotsi::MessageFrame& frame);
    void route_message(luotsi::MessageFrame& frame, const std::string& source_id);
    void reload_config();
    void reconcile_adapters(const Config& new_config);
    void sanitize_payload(nlohmann::json& payload, size_t limit);
    void await_signal();

    // Policy Helpers
    std::string get_active_role_name(const MessageFrame& frame, const std::string& source_id);
    bool is_tool_allowed(const std::string& role_name, const std::string& tool_name);
    bool is_resource_allowed(const std::string& role_name, const std::string& resource_uri);

    // Dependency orchestration
    bool is_dependency_satisfied(const NodeConfig& node, const Config& current_config);
    void check_deferred_nodes(const Config& current_config);
    void spawn_node(const NodeConfig& node, const Config& current_config);

    std::string config_path_;
    Config config_;
    asio::io_context io_context_;
    
    // The Ports: Boundary between Internal and external
    std::map<std::string, std::shared_ptr<ports::IPort>> ports_;

    std::vector<std::string> deferred_nodes_; 
    std::map<std::string, std::string> pending_requests_; 
    std::map<std::string, nlohmann::json> original_ids_;  
    std::map<std::string, nlohmann::json> request_payloads_; 
    std::map<std::string, PendingAggregation> pending_aggregations_; 
    
    std::vector<PolicyRole> roles_; 
    std::string master_node_id_; // ID of the master (catch-all) node, if configured
    std::string session_memory_node_id_; // ID of the session memory observer, if configured
    
    asio::signal_set signals_;
    std::unique_ptr<Observability> observability_;
};

} // namespace luotsi

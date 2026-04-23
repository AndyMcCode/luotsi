#pragma once

#include <string>
#include <map>
#include <deque>
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
    bool is_authorized(const std::string& role_name, const std::string& method);
    bool is_tool_allowed(const std::string& role_name, const std::string& tool_name);
    bool is_resource_allowed(const std::string& role_name, const std::string& resource_uri);

    // Dependency orchestration
    bool is_dependency_satisfied(const NodeConfig& node, const Config& current_config);
    void check_deferred_nodes(const Config& current_config);
    void spawn_node(const NodeConfig& node, const Config& current_config);
    luotsi::NatEntry* find_most_likely_parent(const std::string& node_id);

    std::string config_path_;
    Config config_;
    asio::io_context io_context_;
    
    // The Ports: Boundary between Internal and external
    std::map<std::string, std::shared_ptr<ports::IPort>> ports_;

    std::vector<std::string> deferred_nodes_; 

    // ── Consolidated NAT table (replaces 3 separate maps) ────────────────────
    // Entries are marked fulfilled on response delivery but NOT immediately erased.
    // nat_gc() compacts them lazily once NAT_GC_THRESHOLD is exceeded.
    std::map<std::string, luotsi::NatEntry> nat_table_;
    std::deque<std::string>                 nat_insertion_order_;

    std::map<std::string, PendingAggregation> pending_aggregations_; 
    
    std::vector<PolicyRole> roles_; 
    std::string master_node_id_; // ID of the master (catch-all) node, if configured
    std::string session_memory_node_id_; // ID of the session memory observer, if configured
    
    
    // --- Session Persistence (Continuous distributed cycles across messages) ---
    // Maps user_id (e.g. params.from) → last active trace context.
    // This pins consecutive user messages to the same root trace ID.
    std::map<std::string, luotsi::SessionTrace> session_trace_cache_;

    asio::signal_set signals_;
    std::unique_ptr<Observability> observability_;
};

} // namespace luotsi

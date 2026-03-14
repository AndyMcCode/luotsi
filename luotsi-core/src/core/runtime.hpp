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
    void route_message(luotsi::MessageFrame& frame, const std::string& source_id);
    void reload_config();
    void reconcile_adapters(const Config& new_config);
    void await_signal();

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
    std::map<std::string, PendingAggregation> pending_aggregations_; 
    
    std::vector<PolicyRole> roles_; 
    
    asio::signal_set signals_;
    std::unique_ptr<Observability> observability_;
};

} // namespace luotsi

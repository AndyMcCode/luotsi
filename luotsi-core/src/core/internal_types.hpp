#pragma once

#include <string>
#include <vector>
#include <set>
#include <nlohmann/json.hpp>

namespace luotsi {

struct PendingAggregation {
    std::string source_id;
    nlohmann::json original_id;
    std::string method;
    nlohmann::json original_request;
    std::set<std::string> pending_targets;
    std::vector<nlohmann::json> responses; // Stored results
};

} // namespace luotsi

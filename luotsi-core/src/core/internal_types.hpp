#pragma once

#include <string>
#include <vector>
#include <set>
#include <nlohmann/json.hpp>
#include <chrono>

namespace luotsi {

struct PendingAggregation {
    std::string source_id;
    nlohmann::json original_id;
    std::string method;
    nlohmann::json original_request;
    std::set<std::string> pending_targets;
    std::vector<nlohmann::json> responses; // Stored results
};

struct PendingRequestState {
    std::string source_id;
    std::string trace_id;
    std::string span_id;
    std::string parent_span_id;
    std::chrono::steady_clock::time_point start_time;
};

} // namespace luotsi

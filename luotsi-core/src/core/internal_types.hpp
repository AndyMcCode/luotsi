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
    // Trace context so the aggregation-completion span carries the root trace
    std::string trace_id;
    std::string span_id;
    std::chrono::steady_clock::time_point start_time;
};

struct PendingRequestState {
    std::string source_id;
    std::string trace_id;
    std::string span_id;
    std::string parent_span_id;
    std::chrono::steady_clock::time_point start_time;
};

// ── Consolidated NAT entry (replaces 3 separate maps) ────────────────────────
// Entries are NOT erased on response delivery; instead they are marked
// `fulfilled = true` and collected lazily by Runtime::nat_gc() once the
// table exceeds NAT_GC_THRESHOLD entries.  This keeps recently-completed
// trace contexts available for multi-step session correlation.
struct NatEntry {
    std::string        source_id;
    std::string        target_id;
    nlohmann::json     original_id;
    nlohmann::json     request_payload;
    std::string        trace_id;
    std::string        span_id;
    std::string        parent_span_id;
    std::chrono::steady_clock::time_point start_time;
    bool               fulfilled = false; // true once response was routed back
};

struct SessionTrace {
    std::string trace_id;
    std::string span_id;
    std::chrono::steady_clock::time_point last_seen;
};

} // namespace luotsi

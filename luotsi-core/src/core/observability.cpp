#include "observability.hpp"
#include "internal_types.hpp"
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <random>

namespace luotsi::internal {

std::string generate_uuid_v4() {
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    static std::uniform_int_distribution<uint64_t> dis;

    uint64_t part1 = dis(gen);
    uint64_t part2 = dis(gen);

    // Apply version and variant bits for UUID v4
    part1 = (part1 & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
    part2 = (part2 & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;

    std::stringstream ss;
    ss << std::hex << std::setfill('0')
       << std::setw(8) << (part1 >> 32) << "-"
       << std::setw(4) << ((part1 >> 16) & 0xFFFF) << "-"
       << std::setw(4) << (part1 & 0xFFFF) << "-"
       << std::setw(4) << (part2 >> 48) << "-"
       << std::setw(12) << (part2 & 0xFFFFFFFFFFFFULL);
    return ss.str();
}

std::string current_time_iso8601() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::gmtime(&time), "%FT%TZ");
    return ss.str();
}

Observability::Observability(const std::string& log_path, const std::string& endpoint) : log_path_(log_path), endpoint_(endpoint) {
    if (!log_path.empty()) {
        log_stream_.open(log_path, std::ios::app);
        if (!log_stream_.is_open()) {
            spdlog::error("Failed to open audit log file: {}", log_path);
        } else {
            spdlog::info("Audit logging enabled to: {}", log_path);
        }
    }
    init_udp();
}

Observability::~Observability() {
    if (log_stream_.is_open()) {
        log_stream_.close();
    }
}

void Observability::init_udp() {
    if (endpoint_.empty()) return;
    try {
        size_t colon_pos = endpoint_.find(':');
        if (colon_pos == std::string::npos) return;
        std::string host = endpoint_.substr(0, colon_pos);
        int port = std::stoi(endpoint_.substr(colon_pos + 1));

        io_context_ = std::make_unique<asio::io_context>();
        udp_socket_ = std::make_unique<asio::ip::udp::socket>(*io_context_);
        udp_socket_->open(asio::ip::udp::v4());
        udp_endpoint_ = asio::ip::udp::endpoint(asio::ip::address::from_string(host), port);
        spdlog::info("Observability UDP emitter enabled to {}", endpoint_);
    } catch (const std::exception& e) {
        spdlog::error("Failed to init UDP observability: {}", e.what());
    }
}

void Observability::emit_udp(const std::string& payload) {
    if (udp_socket_ && udp_socket_->is_open()) {
        std::error_code ec;
        udp_socket_->send_to(asio::buffer(payload), udp_endpoint_, 0, ec);
    }
}

void Observability::log_message(const MessageFrame& frame) {
    if (!log_stream_.is_open() && !udp_socket_) return;

    // CloudEvent 1.0 Structure
    nlohmann::json cloudevent;
    cloudevent["specversion"] = "1.0";
    cloudevent["type"] = "luotsi.message";
    cloudevent["source"] = "luotsi-core";
    cloudevent["id"] = generate_uuid_v4();
    cloudevent["time"] = current_time_iso8601();
    cloudevent["datacontenttype"] = "application/json";
    
    if (!frame.source_id.empty()) cloudevent["luotsisource"] = frame.source_id;
    if (!frame.target_id.empty()) cloudevent["luotsitarget"] = frame.target_id;
    
    cloudevent["data"] = {
        {"delegated_role", frame.delegated_role},
        {"payload", frame.payload}
    };

    std::string dump = cloudevent.dump();
    emit_udp(dump);

    if (log_stream_.is_open()) {
        std::lock_guard<std::mutex> lock(mutex_);
        log_stream_ << dump << std::endl;
    }
}

void Observability::log_span(const luotsi::PendingRequestState& req_state, const MessageFrame& response, long long duration_ms) {
    if (!log_stream_.is_open() && !udp_socket_) return;

    nlohmann::json cloudevent;
    cloudevent["specversion"] = "1.0";
    cloudevent["type"] = "luotsi.telemetry.span";
    cloudevent["source"] = "luotsi-core";
    cloudevent["id"] = generate_uuid_v4();
    cloudevent["time"] = current_time_iso8601();
    
    // Core OTel CloudEvent Trace bindings
    if (!req_state.trace_id.empty() && !req_state.span_id.empty()) {
        cloudevent["traceparent"] = "00-" + req_state.trace_id + "-" + req_state.span_id + "-01";
    }

    cloudevent["datacontenttype"] = "application/json";

    nlohmann::json attributes = {
        {"gen_ai.system", "luotsi_switch_fabric"},
        {"gen_ai.agent.name", req_state.source_id},
        {"rpc.system", "jsonrpc"},
        {"rpc.service", response.source_id}
    };

    if (response.payload.contains("error")) {
        attributes["rpc.jsonrpc.error_code"] = response.payload["error"].contains("code") ? response.payload["error"]["code"].get<int>() : -32000;
        cloudevent["data"] = {
            {"name", "luotsi.route"},
            {"kind", "SERVER"},
            {"duration_ms", duration_ms},
            {"status", "ERROR"},
            {"attributes", attributes}
        };
    } else {
        cloudevent["data"] = {
            {"name", "luotsi.route"},
            {"kind", "SERVER"},
            {"duration_ms", duration_ms},
            {"status", "OK"},
            {"attributes", attributes}
        };
    }

    std::string dump = cloudevent.dump();
    emit_udp(dump);

    if (log_stream_.is_open()) {
        std::lock_guard<std::mutex> lock(mutex_);
        log_stream_ << dump << std::endl;
    }
}

} // namespace luotsi::internal

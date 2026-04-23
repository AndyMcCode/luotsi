#pragma once

#include <string>
#include <fstream>
#include <mutex>
#include <memory>
#include <asio.hpp>
#include "../adapters/adapter.hpp"

namespace luotsi {
    struct PendingRequestState;
}

namespace luotsi::internal {

using MessageFrame = luotsi::MessageFrame;

std::string generate_uuid_v4();
std::string generate_trace_id();
std::string generate_span_id();
std::string current_time_iso8601();

class Observability {
public:
    Observability(const std::string& log_path, const std::string& endpoint = "");
    ~Observability();

    // Emit a CloudEvent for a single message hop (source → target).
    void log_message(const MessageFrame& frame);

    // Emit a span CloudEvent when a pending request/response round-trip completes.
    void log_span(const PendingRequestState& req_state, const MessageFrame& response_frame, long long duration_ms, bool is_trace_end = false);

    // Emit a span CloudEvent for a completed fan-out/aggregation.
    void log_aggregation_span(
        const std::string& trace_id,
        const std::string& span_id,
        const std::string& requester_id,
        const std::string& method,
        size_t             targets_count,
        long long          duration_ms,
        bool               has_error);

private:
   std::string log_path_;
   std::ofstream log_stream_;
   std::mutex mutex_;
   
   std::string endpoint_;
   std::unique_ptr<asio::io_context> io_context_;
   std::unique_ptr<asio::ip::udp::socket> udp_socket_;
   asio::ip::udp::endpoint udp_endpoint_;
   
   void init_udp();
   void emit_udp(const std::string& payload);
};

} // namespace luotsi::internal

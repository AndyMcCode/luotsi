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
std::string current_time_iso8601();

class Observability {
public:
    Observability(const std::string& log_path, const std::string& endpoint = "");
    ~Observability();

    void log_message(const MessageFrame& frame);
    void log_span(const PendingRequestState& req_state, const MessageFrame& response_frame, long long duration_ms);

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

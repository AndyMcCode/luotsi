#include "jsonrpc_tcp_adapter.hpp"
#include <spdlog/spdlog.h>
#include <iostream>
#include "../core/observability.hpp"

namespace luotsi::adapters {

JsonRpcTcpAdapter::JsonRpcTcpAdapter(asio::io_context& io_context, std::string node_id)
    : io_context_(io_context), node_id_(node_id), socket_(io_context) {}

JsonRpcTcpAdapter::~JsonRpcTcpAdapter() {
    stop();
}

void JsonRpcTcpAdapter::init(const luotsi::internal::NodeConfig& config, const std::vector<luotsi::internal::PolicyRole>& roles) {
    config_ = config;
    roles_ = roles;
    if (!config_.role.empty()) {
        adapter_role_ = config_.role;
        state_ = SessionState::ESTABLISHED;
    }
}

void JsonRpcTcpAdapter::start() {
    spdlog::info("JsonRpcTcpAdapter '{}' starting connection to {}:{}", node_id_, config_.runtime.host, config_.runtime.port);
    do_connect();
}

void JsonRpcTcpAdapter::stop() {
    asio::error_code ec;
    socket_.close(ec);
    is_connected_ = false;
    spdlog::info("JsonRpcTcpAdapter '{}' stopped", node_id_);
}

void JsonRpcTcpAdapter::send(const MessageFrame& frame) {
    std::string data = frame.payload.dump() + "\n"; // Newline delimited
    
    asio::post(io_context_, [this, data]() {
        bool write_in_progress = !write_queue_.empty();
        write_queue_.push_back(data);
        if (is_connected_ && !write_in_progress) {
            do_write();
        }
    });
}

void JsonRpcTcpAdapter::set_on_receive(OnReceiveCallback callback) {
    on_receive_ = callback;
}

void JsonRpcTcpAdapter::do_connect() {
    asio::ip::tcp::resolver resolver(io_context_);
    auto endpoints = resolver.resolve(config_.runtime.host, std::to_string(config_.runtime.port));

    asio::async_connect(socket_, endpoints,
        [this](std::error_code ec, asio::ip::tcp::endpoint) {
            if (!ec) {
                spdlog::info("JsonRpcTcpAdapter '{}' connected to {}:{}", node_id_, config_.runtime.host, config_.runtime.port);
                is_connected_ = true;
                do_read();
                if (!write_queue_.empty()) {
                    do_write();
                }
            } else {
                spdlog::error("JsonRpcTcpAdapter '{}' connection failed: {}", node_id_, ec.message());
                // Retry? For now just stop.
            }
        });
}

void JsonRpcTcpAdapter::do_read() {
    asio::async_read_until(socket_, read_buffer_, "\n",
        [this](std::error_code ec, std::size_t length) {
            if (!ec) {
                std::istream is(&read_buffer_);
                std::string line;
                std::getline(is, line);

                try {
                    auto rpc = nlohmann::json::parse(line);
                    
                    if (state_ == SessionState::AUTHENTICATING) {
                        if (rpc.contains("method") && rpc["method"] == "initialize") {
                            std::string auth_key;
                            if (rpc.contains("params") && rpc["params"].contains("_meta") && rpc["params"]["_meta"].contains("luotsi_auth")) {
                                auth_key = rpc["params"]["_meta"]["luotsi_auth"].get<std::string>();
                            }
                            
                            bool valid = false;
                            for (const auto& r : roles_) {
                                if (r.secret_key == auth_key) {
                                    adapter_role_ = r.name;
                                    valid = true;
                                    break;
                                }
                            }
                            
                            if (!valid) {
                                spdlog::warn("JsonRpcTcpAdapter '{}' auth failed with key: '{}'", node_id_, auth_key);
                                nlohmann::json err;
                                err["jsonrpc"] = "2.0";
                                err["id"] = rpc.contains("id") ? rpc["id"] : nullptr;
                                err["error"] = {{"code", -32000}, {"message", "Authentication Failed"}};
                                std::string data = err.dump() + "\n";
                                
                                bool write_in_progress = !write_queue_.empty();
                                write_queue_.push_back(data);
                                if (is_connected_ && !write_in_progress) {
                                    do_write();
                                }
                                asio::post(io_context_, [this]() {
                                    stop();
                                });
                                return;
                            } else {
                                spdlog::info("JsonRpcTcpAdapter '{}' authenticated as role '{}'", node_id_, adapter_role_);
                                state_ = SessionState::ESTABLISHED;
                                
                                MessageFrame frame;
                                frame.source_id = node_id_;
                                frame.payload = rpc;
                                
                                std::string traceparent = "";
                                if (rpc.contains("_meta") && rpc["_meta"].contains("traceparent")) {
                                    traceparent = rpc["_meta"]["traceparent"].get<std::string>();
                                } else if (rpc.contains("params") && rpc["params"].is_object() && rpc["params"].contains("_meta") && rpc["params"]["_meta"].contains("traceparent")) {
                                    traceparent = rpc["params"]["_meta"]["traceparent"].get<std::string>();
                                }

                                if (!traceparent.empty() && traceparent.size() >= 55) {
                                    frame.trace_id = traceparent.substr(3, 32);
                                    frame.parent_span_id = traceparent.substr(36, 16);
                                } else {
                                    std::string uuid = luotsi::internal::generate_uuid_v4();
                                    uuid.erase(std::remove(uuid.begin(), uuid.end(), '-'), uuid.end());
                                    frame.trace_id = uuid;
                                }
                                
                                std::string span_id = luotsi::internal::generate_uuid_v4();
                                span_id.erase(std::remove(span_id.begin(), span_id.end(), '-'), span_id.end());
                                frame.span_id = span_id.substr(0, 16);
                                frame.timestamp = std::chrono::steady_clock::now();

                                if (on_receive_) on_receive_(frame);
                            }
                        } else {
                            spdlog::warn("JsonRpcTcpAdapter '{}' expected initialize, got: {}", node_id_, rpc.dump());
                            stop();
                            return;
                        }
                    } else {
                        // ESTABLISHED
                        MessageFrame frame;
                        frame.source_id = node_id_;
                        frame.payload = rpc;

                        std::string traceparent = "";
                        if (rpc.contains("_meta") && rpc["_meta"].contains("traceparent")) {
                            traceparent = rpc["_meta"]["traceparent"].get<std::string>();
                        } else if (rpc.contains("params") && rpc["params"].is_object() && rpc["params"].contains("_meta") && rpc["params"]["_meta"].contains("traceparent")) {
                            traceparent = rpc["params"]["_meta"]["traceparent"].get<std::string>();
                        }

                        if (!traceparent.empty() && traceparent.size() >= 55) {
                            frame.trace_id = traceparent.substr(3, 32);
                            frame.parent_span_id = traceparent.substr(36, 16);
                        } else {
                            std::string uuid = luotsi::internal::generate_uuid_v4();
                            uuid.erase(std::remove(uuid.begin(), uuid.end(), '-'), uuid.end());
                            frame.trace_id = uuid;
                        }
                        
                        std::string span_id = luotsi::internal::generate_uuid_v4();
                        span_id.erase(std::remove(span_id.begin(), span_id.end(), '-'), span_id.end());
                        frame.span_id = span_id.substr(0, 16);
                        frame.timestamp = std::chrono::steady_clock::now();

                        if (on_receive_) on_receive_(frame);
                    }
                } catch (const std::exception& e) {
                    spdlog::error("JsonRpcTcpAdapter '{}' failed to parse received JSON: {}", node_id_, e.what());
                }

                do_read();
            } else {
                spdlog::error("JsonRpcTcpAdapter '{}' read error: {}", node_id_, ec.message());
                is_connected_ = false;
            }
        });
}

void JsonRpcTcpAdapter::do_write() {
    asio::async_write(socket_, asio::buffer(write_queue_.front()),
        [this](std::error_code ec, std::size_t /*length*/) {
            if (!ec) {
                write_queue_.pop_front();
                if (!write_queue_.empty()) {
                    do_write();
                }
            } else {
                spdlog::error("JsonRpcTcpAdapter '{}' write error: {}", node_id_, ec.message());
                is_connected_ = false;
            }
        });
}

} // namespace luotsi

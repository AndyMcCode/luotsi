#include "jsonrpc_tcp_adapter.hpp"
#include <spdlog/spdlog.h>
#include <iostream>

namespace luotsi::adapters {

JsonRpcTcpAdapter::JsonRpcTcpAdapter(asio::io_context& io_context, std::string node_id)
    : io_context_(io_context), node_id_(node_id), socket_(io_context) {}

JsonRpcTcpAdapter::~JsonRpcTcpAdapter() {
    stop();
}

void JsonRpcTcpAdapter::init(const luotsi::internal::RuntimeConfig& config) {
    config_ = config;
}

void JsonRpcTcpAdapter::start() {
    spdlog::info("JsonRpcTcpAdapter '{}' starting connection to {}:{}", node_id_, config_.host, config_.port);
    do_connect();
}

void JsonRpcTcpAdapter::stop() {
    asio::error_code ec;
    socket_.close(ec);
    is_connected_ = false;
    spdlog::info("JsonRpcTcpAdapter '{}' stopped", node_id_);
}

void JsonRpcTcpAdapter::send(const MessageFrame& frame) {
    // 1. Wrap in JSON-RPC 2.0 Notification
    nlohmann::json rpc;
    rpc["jsonrpc"] = "2.0";
    rpc["method"] = "luotsi.forward";
    rpc["params"] = {
        {"source_id", frame.source_id},
        {"target_id", frame.target_id},
        {"payload", frame.payload}
    };

    std::string data = rpc.dump() + "\n"; // Newline delimited
    
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
    auto endpoints = resolver.resolve(config_.host, std::to_string(config_.port));

    asio::async_connect(socket_, endpoints,
        [this](std::error_code ec, asio::ip::tcp::endpoint) {
            if (!ec) {
                spdlog::info("JsonRpcTcpAdapter '{}' connected to {}:{}", node_id_, config_.host, config_.port);
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
                    if (rpc.contains("method") && rpc["method"] == "luotsi.forward") {
                        auto params = rpc["params"];
                        MessageFrame frame;
                        frame.source_id = params["source_id"].get<std::string>();
                        frame.target_id = params["target_id"].get<std::string>();
                        frame.payload = params["payload"];
                        
                        if (on_receive_) {
                            on_receive_(frame);
                        }
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

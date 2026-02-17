#pragma once

#include "adapter.hpp"
#include <asio.hpp>
#include <thread>
#include <deque>

namespace luotsi {

class JsonRpcTcpAdapter : public IAdapter {
public:
    JsonRpcTcpAdapter(asio::io_context& io_context, std::string node_id);
    ~JsonRpcTcpAdapter();

    void init(const RuntimeConfig& config) override;
    void start() override;
    void stop() override;
    void send(const MessageFrame& frame) override;
    void set_on_receive(OnReceiveCallback callback) override;

private:
    void do_connect();
    void do_read();
    void do_write();

    asio::io_context& io_context_;
    std::string node_id_;
    RuntimeConfig config_;
    OnReceiveCallback on_receive_;

    asio::ip::tcp::socket socket_;
    asio::streambuf read_buffer_;
    std::deque<std::string> write_queue_;
    bool is_connected_ = false;
};

} // namespace luotsi

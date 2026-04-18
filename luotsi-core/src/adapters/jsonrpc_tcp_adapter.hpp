#pragma once

#include "adapter.hpp"
#include <asio.hpp>
#include <thread>
#include <deque>

namespace luotsi::adapters {

class JsonRpcTcpAdapter : public IAdapter {
public:
    JsonRpcTcpAdapter(asio::io_context& io_context, std::string node_id);
    ~JsonRpcTcpAdapter();

    void init(const luotsi::internal::NodeConfig& config, const std::vector<luotsi::internal::PolicyRole>& roles) override;
    void start() override;
    void stop() override;
    
    std::string get_adapter_role() const override { return adapter_role_; }
    bool is_authenticated() const override { return state_ == SessionState::ESTABLISHED; }

    void send(const MessageFrame& frame) override;
    void set_on_receive(OnReceiveCallback callback) override;

private:
    void do_connect();
    void do_read();
    void do_write();

    asio::io_context& io_context_;
    std::string node_id_;
    luotsi::internal::NodeConfig config_;
    std::vector<luotsi::internal::PolicyRole> roles_;
    OnReceiveCallback on_receive_;

    asio::ip::tcp::socket socket_;
    asio::streambuf read_buffer_;
    std::deque<std::string> write_queue_;
    bool is_connected_ = false;

    enum class SessionState { AUTHENTICATING, ESTABLISHED };
    SessionState state_ = SessionState::AUTHENTICATING;
    std::string adapter_role_;
};

} // namespace luotsi

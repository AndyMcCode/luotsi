#pragma once

#include "adapter.hpp"
#include <asio.hpp>
#include <thread>
#include <vector>

#if defined(__linux__) || defined(__APPLE__)
#include <unistd.h>
#else
#error "Platform not supported for StdioAdapter"
#endif

namespace luotsi::adapters {

class StdioAdapter : public IAdapter {
public:
    StdioAdapter(asio::io_context& io_context, std::string node_id);
    ~StdioAdapter();

    void init(const luotsi::internal::NodeConfig& config, const std::vector<luotsi::internal::PolicyRole>& roles) override;
    void start() override;
    void stop() override;
    
    std::string get_adapter_role() const override { return adapter_role_; }
    bool is_authenticated() const override { return !adapter_role_.empty(); }

    void send(const MessageFrame& frame) override;
    void set_on_receive(OnReceiveCallback callback) override;

private:
    void read_stdout(); // Async read loop
    void read_stderr(); // Async read loop for stderr
    void spawn_process();

    asio::io_context& io_context_;
    std::string node_id_;
    luotsi::internal::NodeConfig config_;
    std::string adapter_role_;
    OnReceiveCallback on_receive_;

    // Process handles
    pid_t pid_ = -1;
    int pipe_stdin_[2];  
    int pipe_stdout_[2]; 
    int pipe_stderr_[2]; 

    // Asio stream/descriptor wrapper
    std::unique_ptr<asio::posix::stream_descriptor> stdout_stream_;
    std::unique_ptr<asio::posix::stream_descriptor> stderr_stream_;
    asio::streambuf input_buffer_;
    asio::streambuf stderr_buffer_;
};

} // namespace luotsi::adapters

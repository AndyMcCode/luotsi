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

namespace luotsi {

class StdioAdapter : public IAdapter {
public:
    StdioAdapter(asio::io_context& io_context, std::string node_id);
    ~StdioAdapter();

    void init(const RuntimeConfig& config) override;
    void start() override;
    void stop() override;
    void send(const MessageFrame& frame) override;
    void set_on_receive(OnReceiveCallback callback) override;

private:
    void read_stdout(); // Async read loop
    void spawn_process();

    asio::io_context& io_context_;
    std::string node_id_;
    RuntimeConfig config_;
    OnReceiveCallback on_receive_;

    // Process handles
    pid_t pid_ = -1;
    int pipe_stdin_[2];  // Parent writes to [1], Child reads from [0]
    int pipe_stdout_[2]; // Parent reads from [0], Child writes to [1]

    // Asio stream/descriptor wrapper
    std::unique_ptr<asio::posix::stream_descriptor> stdout_stream_;
    asio::streambuf input_buffer_;
};

} // namespace luotsi

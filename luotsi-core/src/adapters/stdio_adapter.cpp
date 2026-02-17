#include "stdio_adapter.hpp"
#include <spdlog/spdlog.h>
#include <sys/wait.h>
#include <iostream>

namespace luotsi {

StdioAdapter::StdioAdapter(asio::io_context& io_context, std::string node_id)
    : io_context_(io_context), node_id_(std::move(node_id)) {}

StdioAdapter::~StdioAdapter() {
    stop();
}

void StdioAdapter::init(const RuntimeConfig& config) {
    config_ = config;
}

void StdioAdapter::start() {
    spawn_process();
    // Start async read from stdout_stream
    stdout_stream_ = std::make_unique<asio::posix::stream_descriptor>(io_context_, pipe_stdout_[0]);
    read_stdout();
}

void StdioAdapter::stop() {
    if (pid_ > 0) {
        spdlog::info("Stopping node {}, killing pid {}", node_id_, pid_);
        kill(pid_, SIGTERM);
        int status;
        waitpid(pid_, &status, 0);
        pid_ = -1;
    }
    if (stdout_stream_) {
        stdout_stream_->close();
        stdout_stream_.reset();
    }
    // Closes pipes if not already closed by stream_descriptor
    // (stream_descriptor takes ownership if we constructed it with the fd, 
    // but here we might need to be careful not to double close or leak if constructor throws)
    // For simplicity in this skeleton, we assume stream_descriptor manages the read end.
    // The write end of stdin pipe needs explicit closing
    close(pipe_stdin_[1]);
}

void StdioAdapter::spawn_process() {
    if (pipe(pipe_stdin_) == -1 || pipe(pipe_stdout_) == -1) {
        throw std::runtime_error("Failed to create pipes");
    }

    pid_ = fork();
    if (pid_ < 0) {
        throw std::runtime_error("Failed to fork");
    }

    if (pid_ == 0) {
        // Child
        // Close unused ends
        close(pipe_stdin_[1]); // Child reads from stdin[0]
        close(pipe_stdout_[0]); // Child writes to stdout[1]

        // Redirect Stdin/Stdout
        if (dup2(pipe_stdin_[0], STDIN_FILENO) == -1 || 
            dup2(pipe_stdout_[1], STDOUT_FILENO) == -1) {
            perror("dup2");
            exit(1);
        }

        // Close originals after dup
        close(pipe_stdin_[0]);
        close(pipe_stdout_[1]);
        
        // Prepare args
        std::vector<const char*> args;
        args.push_back(config_.command.c_str());
        for (const auto& arg : config_.args) {
            args.push_back(arg.c_str());
        }
        args.push_back(nullptr);

        execvp(config_.command.c_str(), const_cast<char* const*>(args.data()));
        // If execvp returns, it failed
        perror("execvp");
        exit(1);
    } else {
        // Parent
        // Close unused ends
        close(pipe_stdin_[0]); // Parent writes to stdin[1]
        close(pipe_stdout_[1]); // Parent reads from stdout[0]
        
        spdlog::info("Spawned node {} with pid {}", node_id_, pid_);
    }
}

void StdioAdapter::send(const MessageFrame& frame) {
    // Parent writes to Child's Stdin
    // Naively writing sync for now or we could use asio::async_write if we wrap pipe_stdin_[1] in a stream_descriptor
    // For skeleton, let's use sync write to the pipe fd just to get it working, 
    // or better: wrap it since IAdapter interface might be called from IO thread.
    
    // Format: Line delimited JSON
    std::string data = frame.payload.dump() + "\n";
    ssize_t written = ::write(pipe_stdin_[1], data.c_str(), data.size());
    if (written < 0) {
        spdlog::error("Failed to write to node {}", node_id_);
    }
}

void StdioAdapter::read_stdout() {
    // Read until newline
    asio::async_read_until(*stdout_stream_, input_buffer_, '\n',
        [this](const std::error_code& error, std::size_t bytes_transferred) {
            if (!error) {
                std::istream is(&input_buffer_);
                std::string line;
                std::getline(is, line);
                
                if (!line.empty()) {
                    try {
                        auto json = nlohmann::json::parse(line);
                        // Construct Frame
                        MessageFrame frame;
                        frame.source_id = node_id_;
                        // Target? parsed from frame or routing table?
                        // For PoC, let's assume the agent sends full envelope or we fill it.
                        // If agent sends just payload, we need logic.
                        // Let's assume agent sends {"method": ...} (JsonRPC)
                        // This adapter wraps it into a MessageFrame for the bus.
                        frame.payload = json;
                        
                        if (on_receive_) {
                            on_receive_(frame);
                        }
                    } catch (const std::exception& e) {
                        spdlog::warn("Node {} emitted invalid JSON: {}", node_id_, line);
                    }
                }
                
                read_stdout(); // Continue reading
            } else {
                if (error != asio::error::operation_aborted) {
                    spdlog::error("Node {} read error: {}", node_id_, error.message());
                }
            }
        });
}

void StdioAdapter::set_on_receive(OnReceiveCallback callback) {
    on_receive_ = callback;
}

} // namespace luotsi

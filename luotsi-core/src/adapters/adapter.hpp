#pragma once

#include <string>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include "../core/config.hpp"

#include <chrono>

namespace luotsi {
struct MessageFrame {
    std::string source_id;
    std::string target_id;
    nlohmann::json payload;
    std::string trace_id;
    std::string span_id;
    std::string parent_span_id;
    std::chrono::steady_clock::time_point timestamp;
};
}

namespace luotsi::adapters {
using MessageFrame = luotsi::MessageFrame;

class IAdapter {
public:
    virtual ~IAdapter() = default;

    // Initialize with specific config (e.g. command args) and roles for IAM
    virtual void init(const luotsi::internal::NodeConfig& config, const std::vector<luotsi::internal::PolicyRole>& roles) = 0;

    // Start the adapter (e.g. spawn process, bind port)
    virtual void start() = 0;

    // Stop/Cleanup
    virtual void stop() = 0;

    // Retrieve authenticated identity state dynamically
    virtual std::string get_adapter_role() const = 0;
    virtual bool is_authenticated() const = 0;

    // Send a message TO the adapter (outbound from Core)
    virtual void send(const MessageFrame& frame) = 0;

    // Callback for when adapter receives data FROM the external world
    using OnReceiveCallback = std::function<void(MessageFrame)>;
    virtual void set_on_receive(OnReceiveCallback callback) = 0;
};

} // namespace luotsi

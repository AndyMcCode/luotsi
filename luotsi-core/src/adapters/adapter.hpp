#pragma once

#include <string>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include "../core/config.hpp"

namespace luotsi {
struct MessageFrame {
    std::string source_id;
    std::string target_id;
    nlohmann::json payload;
};
}

namespace luotsi::adapters {
using MessageFrame = luotsi::MessageFrame;

class IAdapter {
public:
    virtual ~IAdapter() = default;

    // Initialize with specific config (e.g. command args)
    virtual void init(const luotsi::internal::RuntimeConfig& config) = 0;

    // Start the adapter (e.g. spawn process, bind port)
    virtual void start() = 0;

    // Stop/Cleanup
    virtual void stop() = 0;

    // Send a message TO the adapter (outbound from Core)
    virtual void send(const MessageFrame& frame) = 0;

    // Callback for when adapter receives data FROM the external world
    using OnReceiveCallback = std::function<void(MessageFrame)>;
    virtual void set_on_receive(OnReceiveCallback callback) = 0;
};

} // namespace luotsi

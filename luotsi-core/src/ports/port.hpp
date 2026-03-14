#pragma once

#include <string>
#include <memory>
#include <functional>
#include "../adapters/adapter.hpp"

namespace luotsi::ports {

/**
 * @brief Base interface for all Ports.
 * Ports act as the boundary between the Internal Core and the External Adapters.
 */
class IPort {
public:
    virtual ~IPort() = default;

    // Outbound: Core -> External (via Adapter)
    virtual void send(const luotsi::MessageFrame& frame) = 0;

    // Inbound: External -> Core (via Adapter)
    using OnReceiveCallback = std::function<void(luotsi::MessageFrame)>;
    virtual void set_on_receive(OnReceiveCallback callback) = 0;
    
    virtual std::string get_id() const = 0;
};

} // namespace luotsi::ports

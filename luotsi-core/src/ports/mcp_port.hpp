#pragma once

#include "port.hpp"

namespace luotsi::ports {

/**
 * @brief Port for MCP Server communication.
 * Handles auto-discovery and capability caching at the boundary.
 */
class McpPort : public virtual IPort {
public:
    virtual ~McpPort() = default;

    virtual bool isInitialized() const = 0;
    virtual void markInitialized(bool ready) = 0;
    
    // Capability management
    virtual void updateCapabilities(const std::string& type, const nlohmann::json& caps) = 0;
    virtual nlohmann::json getCapabilities(const std::string& type) const = 0;
};

} // namespace luotsi::ports

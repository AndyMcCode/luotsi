#pragma once

#include "port.hpp"

namespace luotsi::ports {

/**
 * @brief Port for Agent communication using ACP (Agent Core Protocol).
 */
class AgentPort : public virtual IPort {
public:
    virtual ~AgentPort() = default;

    // Agent ports might require specialized methods for ACP handshake 
    // or policy enforcement at the entry point.
    virtual bool isAuthenticated() const = 0;
    virtual std::string getRole() const = 0;
    virtual void setRole(const std::string& role) = 0;
};

} // namespace luotsi::ports

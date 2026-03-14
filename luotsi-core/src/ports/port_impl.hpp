#pragma once

#include "agent_port.hpp"
#include "mcp_port.hpp"
#include <iostream>

namespace luotsi::ports {

class GenericPort : public virtual IPort {
protected:
    std::string id_;
    std::shared_ptr<luotsi::adapters::IAdapter> adapter_;
    OnReceiveCallback on_receive_;

public:
    GenericPort(const std::string& id, std::shared_ptr<luotsi::adapters::IAdapter> adapter)
        : id_(id), adapter_(adapter) {
        adapter_->set_on_receive([this](luotsi::MessageFrame frame) {
            if (on_receive_) {
                on_receive_(frame);
            }
        });
    }

    void send(const luotsi::MessageFrame& frame) override {
        adapter_->send(frame);
    }

    void set_on_receive(OnReceiveCallback callback) override {
        on_receive_ = callback;
    }

    std::string get_id() const override {
        return id_;
    }
};

class GenericAgentPort : public GenericPort, public AgentPort {
    std::string role_;
    bool authenticated_ = false;

public:
    GenericAgentPort(const std::string& id, std::shared_ptr<luotsi::adapters::IAdapter> adapter)
        : GenericPort(id, adapter) {}

    bool isAuthenticated() const override { return authenticated_; }
    std::string getRole() const override { return role_; }
    void setRole(const std::string& role) override {
        role_ = role;
        authenticated_ = !role.empty();
    }
};

class GenericMcpPort : public GenericPort, public McpPort {
    bool initialized_ = false;
    nlohmann::json tools_ = nlohmann::json::array();
    nlohmann::json resources_ = nlohmann::json::array();
    nlohmann::json templates_ = nlohmann::json::array();
    nlohmann::json prompts_ = nlohmann::json::array();

public:
    GenericMcpPort(const std::string& id, std::shared_ptr<luotsi::adapters::IAdapter> adapter)
        : GenericPort(id, adapter) {}

    bool isInitialized() const override { return initialized_; }
    void markInitialized(bool ready) override { initialized_ = ready; }

    void updateCapabilities(const std::string& type, const nlohmann::json& caps) override {
        if (type == "tools") tools_ = caps;
        else if (type == "resources") resources_ = caps;
        else if (type == "templates") templates_ = caps;
        else if (type == "prompts") prompts_ = caps;
    }

    nlohmann::json getCapabilities(const std::string& type) const override {
        if (type == "tools") return tools_;
        if (type == "resources") return resources_;
        if (type == "templates") return templates_;
        if (type == "prompts") return prompts_;
        return nlohmann::json::array();
    }
};

} // namespace luotsi::ports

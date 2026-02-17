#pragma once

#include <string>
#include <map>
#include <memory>
#include <asio.hpp>
#include "config.hpp"
#include "../adapters/adapter.hpp"
#include "observability.hpp"

namespace luotsi {

class Runtime {
public:
    Runtime(const std::string& config_path);
    void start();
    void stop(); // Graceful shutdown

private:
    void route_message(MessageFrame& frame, const std::string& source_id);
    void reload_config();
    void reconcile_adapters(const Config& new_config);
    void await_signal();

    std::string config_path_;
    Config config_;
    asio::io_context io_context_;
    std::map<std::string, std::shared_ptr<IAdapter>> adapters_;
    std::map<std::string, std::string> pending_requests_; // id -> original_source_id
    asio::signal_set signals_;
    std::unique_ptr<Observability> observability_;
};

} // namespace luotsi

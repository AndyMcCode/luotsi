#pragma once

#include <string>
#include <fstream>
#include <mutex>
#include "../adapters/adapter.hpp"

namespace luotsi::internal {

using MessageFrame = luotsi::MessageFrame;

class Observability {
public:
    Observability(const std::string& log_path);
    ~Observability();

    void log_message(const MessageFrame& frame);

private:
   std::string log_path_;
   std::ofstream log_stream_;
   std::mutex mutex_;
};

} // namespace luotsi::internal

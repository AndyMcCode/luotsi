#include "observability.hpp"
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <random>

namespace luotsi::internal {

std::string generate_uuid_v4() {
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    static std::uniform_int_distribution<uint64_t> dis;

    uint64_t part1 = dis(gen);
    uint64_t part2 = dis(gen);

    // Apply version and variant bits for UUID v4
    part1 = (part1 & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
    part2 = (part2 & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;

    std::stringstream ss;
    ss << std::hex << std::setfill('0')
       << std::setw(8) << (part1 >> 32) << "-"
       << std::setw(4) << ((part1 >> 16) & 0xFFFF) << "-"
       << std::setw(4) << (part1 & 0xFFFF) << "-"
       << std::setw(4) << (part2 >> 48) << "-"
       << std::setw(12) << (part2 & 0xFFFFFFFFFFFFULL);
    return ss.str();
}

std::string current_time_iso8601() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::gmtime(&time), "%FT%TZ");
    return ss.str();
}

Observability::Observability(const std::string& log_path) : log_path_(log_path) {
    if (!log_path.empty()) {
        log_stream_.open(log_path, std::ios::app);
        if (!log_stream_.is_open()) {
            spdlog::error("Failed to open audit log file: {}", log_path);
        } else {
            spdlog::info("Audit logging enabled to: {}", log_path);
        }
    }
}

Observability::~Observability() {
    if (log_stream_.is_open()) {
        log_stream_.close();
    }
}

void Observability::log_message(const MessageFrame& frame) {
    if (!log_stream_.is_open()) return;

    // CloudEvent 1.0 Structure
    nlohmann::json cloudevent;
    cloudevent["specversion"] = "1.0";
    cloudevent["type"] = "luotsi.message";
    cloudevent["source"] = "luotsi-core";
    cloudevent["id"] = generate_uuid_v4();
    cloudevent["time"] = current_time_iso8601();
    cloudevent["datacontenttype"] = "application/json";
    
    cloudevent["data"] = {
        {"source_id", frame.source_id},
        {"target_id", frame.target_id},
        {"payload", frame.payload}
    };

    std::lock_guard<std::mutex> lock(mutex_);
    log_stream_ << cloudevent.dump() << std::endl;
}

} // namespace luotsi::internal

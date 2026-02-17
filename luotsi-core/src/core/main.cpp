#include <iostream>
#include <spdlog/spdlog.h>
#include "runtime.hpp"

using namespace luotsi;

int main(int argc, char** argv) {
    std::string config_path = "luotsi.config.yaml";
    if (argc >= 3 && std::string(argv[1]) == "--config") {
        config_path = argv[2];
    } else if (argc >= 2) {
         if (std::string(argv[1]) == "--playground") {
             // If running from repo root: ./luotsi-core/build/luotsi --playground
             // or ./build/luotsi --playground
             // The config is in ../playground/configs/luotsi.config.yaml relative to build dir?
             // Or playground/configs/luotsi.config.yaml relative to CWD?
             // Let's assume CWD is repo root
             config_path = "playground/configs/luotsi.config.yaml"; 
         } else {
             config_path = argv[1];
         }
    }

    try {
        Runtime runtime(config_path);
        runtime.start();
    } catch (const std::exception& e) {
        spdlog::critical("Runtime error: {}", e.what());
        return 1;
    }

    return 0;
}

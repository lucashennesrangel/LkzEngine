#include "lkz/Core/Log.hpp"

#include <spdlog/sinks/stdout_color_sinks.h>

#ifdef _WIN32
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#endif

namespace lkz {

void initLogging(spdlog::level::level_enum level) {
    if (spdlog::get("lkz") == nullptr) {
#ifdef _WIN32
        // Os textos são UTF-8; sem isso o console do Windows mostra acentos quebrados.
        SetConsoleOutputCP(CP_UTF8);
#endif
        auto logger = spdlog::stdout_color_mt("lkz");
        logger->set_pattern("[%H:%M:%S.%e] [%^%l%$] [t%t] %v");
        spdlog::set_default_logger(std::move(logger));
    }
    spdlog::set_level(level);
}

} // namespace lkz

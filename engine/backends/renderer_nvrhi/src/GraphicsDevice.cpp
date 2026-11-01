#include "GraphicsDevice.hpp"

#include <spdlog/spdlog.h>

namespace lkz::nvrhi_detail {

namespace {

class LogCallback final : public nvrhi::IMessageCallback {
public:
    void message(nvrhi::MessageSeverity severity, const char* text) override {
        switch (severity) {
        case nvrhi::MessageSeverity::Info: spdlog::info("NVRHI: {}", text); break;
        case nvrhi::MessageSeverity::Warning: spdlog::warn("NVRHI: {}", text); break;
        case nvrhi::MessageSeverity::Error:
        case nvrhi::MessageSeverity::Fatal: spdlog::error("NVRHI: {}", text); break;
        }
    }
};

} // namespace

nvrhi::IMessageCallback* messageCallback() {
    static LogCallback callback;
    return &callback;
}

} // namespace lkz::nvrhi_detail

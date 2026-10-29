#include "lkz/backends/NullRenderBackend.hpp"

#include <spdlog/spdlog.h>

namespace lkz {

bool NullRenderBackend::initialize() {
    spdlog::info("NullRenderBackend inicializado (nada será desenhado)");
    return true;
}

void NullRenderBackend::shutdown() {
    const double drawsPerFrame = frames_ > 0 ? static_cast<double>(draws_) / static_cast<double>(frames_) : 0.0;
    spdlog::info("NullRenderBackend encerrado: {} frames, {:.0f} draws/frame", frames_, drawsPerFrame);
}

void NullRenderBackend::beginFrame(const CameraView& /*camera*/) {
    ++frames_;
}

void NullRenderBackend::submit(std::span<const DrawCommand> commands) {
    draws_ += commands.size();
}

} // namespace lkz

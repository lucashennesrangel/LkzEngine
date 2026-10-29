#pragma once

#include <cstdint>

#include <lkz/Render/RenderBackend.hpp>

namespace lkz {

/// Backend que não desenha nada: só conta frames e draws.
/// Serve para validar o loop, a interpolação e o profiling sem depender de GPU.
class NullRenderBackend final : public RenderBackend {
public:
    bool initialize() override;
    void shutdown() override;

    void beginFrame(const CameraView& camera) override;
    void submit(std::span<const DrawCommand> commands) override;
    void endFrame() override {}

private:
    std::uint64_t frames_ = 0;
    std::uint64_t draws_ = 0;
};

} // namespace lkz

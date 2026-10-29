#pragma once

#include <span>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include "lkz/ECS/Components.hpp"

namespace lkz {

/// Câmera já interpolada, pronta para o backend montar a projeção.
struct CameraView {
    glm::mat4 view{1.0f};
    glm::vec3 position{0.0f};
    float fovY = 1.0471976f;
    float nearPlane = 0.1f;
    float farPlane = 1000.0f;
};

/// Um objeto a desenhar, com a matriz de modelo já interpolada.
struct DrawCommand {
    glm::mat4 model{1.0f};
    RenderComponent render{};
};

/// Interface que cada backend gráfico (null, bgfx, raylib, vulkan...) implementa.
/// Todos os métodos são chamados na thread de render, inclusive initialize/shutdown.
class RenderBackend {
public:
    virtual ~RenderBackend() = default;

    /// Retorna false se o backend não pôde ser inicializado (o loop é encerrado).
    virtual bool initialize() = 0;
    virtual void shutdown() = 0;

    virtual void beginFrame(const CameraView& camera) = 0;
    virtual void submit(std::span<const DrawCommand> commands) = 0;
    virtual void endFrame() = 0;
};

} // namespace lkz

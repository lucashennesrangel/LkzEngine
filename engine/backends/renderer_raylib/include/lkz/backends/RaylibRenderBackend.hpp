#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <lkz/Render/RenderBackend.hpp>
#include <lkz/Render/RenderConfig.hpp>

namespace lkz {

/// O mesmo RenderConfig de qualquer backend com janela (lkz/Render/RenderConfig.hpp).
using RaylibConfig = RenderConfig;

/// Backend com janela (raylib, OpenGL 3.3). Desenha cada id de mesh como uma
/// primitiva, malha do jogo ou modelo 3D (opcionalmente com uma imagem, repetida em
/// grade ou em hex-tiling), com um draw call instanciado por parte e cor por instância.
///
/// A janela é criada e atendida na thread de render (initialize/endFrame); fechar a
/// janela ou apertar ESC faz closeRequested() retornar true e o World encerra o loop.
/// Teclado e mouse chegam à lógica por pollInput() -> World::input().
class RaylibRenderBackend final : public RenderBackend {
public:
    explicit RaylibRenderBackend(RaylibConfig config = {});
    ~RaylibRenderBackend() override;

    RaylibRenderBackend(const RaylibRenderBackend&) = delete;
    RaylibRenderBackend& operator=(const RaylibRenderBackend&) = delete;

    bool initialize() override;
    void shutdown() override;

    void updateMesh(std::uint32_t id, const MeshData& mesh) override;
    void updateInstanceBatch(std::uint32_t id, const InstanceBatch& batch) override;
    void beginFrame(const CameraView& camera) override;
    void submit(std::span<const DrawCommand> commands) override;
    void drawOverlay(std::string_view text) override;
    void setTerrain(const TerrainDesc& terrain) override;
    void updateTerrain(const TerrainPatch& patch) override;
    void uploadTexture(std::uint32_t id, const TextureImage& image) override;
    void updateTexture(std::uint32_t id, const TexturePatch& patch) override;
    void loadUi(const UiDocument& document) override;
    void updateUi(std::span<const std::pair<std::string, UiValue>> values) override;
    void setMouseCursor(MouseCursor cursor) override;
    void endFrame() override;

    [[nodiscard]] bool closeRequested() const override;
    void pollInput(InputState& input) override;

private:
    struct Impl; // guarda os tipos da raylib fora do header público
    std::unique_ptr<Impl> impl_;
};

} // namespace lkz

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <lkz/Render/RenderBackend.hpp>
#include <lkz/Render/RenderConfig.hpp>

namespace lkz {

/// API gráfica do NvrhiRenderBackend.
enum class GraphicsApi : std::uint8_t {
    D3D12,  // DirectX 12 (Windows)
    Vulkan, // Vulkan 1.3 (Windows e Linux)
};

/// "d3d12"/"dx12" ou "vulkan"/"vk" (sem diferenciar maiúsculas); nada para outro texto.
[[nodiscard]] std::optional<GraphicsApi> parseGraphicsApi(std::string_view name);
[[nodiscard]] const char* graphicsApiName(GraphicsApi api);

/// Backend com janela sobre o NVRHI (NVIDIA): DirectX 12 ou Vulkan 1.3, janela e
/// entrada pela SDL3, shaders em HLSL. Usa o mesmo RenderConfig do backend raylib e
/// desenha cada id de mesh como primitiva ou malha do jogo, com imagem (grade comum ou
/// hex-tiling) e um draw call instanciado por parte.
///
/// Terreno CDLOD (World::setTerrain), com F3 (RenderConfig::lodDebugKey) colorindo os
/// níveis, e a interface do World::showUi (RmlUi, a mesma do backend raylib), com F8
/// (RenderConfig::uiDebuggerKey) abrindo o depurador dela.
///
/// Modelos glTF (cgltf): cada primitiva do arquivo vira uma parte, com a imagem e a cor
/// base do material. Lotes de cópias (World::setInstanceBatch) ficam num buffer fixo na
/// GPU, em células do plano XZ; só as células dentro da câmera são desenhadas.
///
/// Em construção: LOD e impostores (desenha sempre o original), texto sobre a cena e
/// MSAA (o backend raylib continua sendo o completo).
///
/// A janela é criada e atendida na thread de render (initialize/endFrame). Profundidade
/// invertida (reversed-Z, 32 bits): precisão boa de perto e de longe em mapas grandes.
class NvrhiRenderBackend final : public RenderBackend {
public:
    /// `debug`: camadas de validação do D3D12/Vulkan e do NVRHI (mais lento; erros no log).
    explicit NvrhiRenderBackend(RenderConfig config = {}, GraphicsApi api = GraphicsApi::D3D12, bool debug = false);
    ~NvrhiRenderBackend() override;

    NvrhiRenderBackend(const NvrhiRenderBackend&) = delete;
    NvrhiRenderBackend& operator=(const NvrhiRenderBackend&) = delete;

    bool initialize() override;
    void shutdown() override;
    void updateMesh(std::uint32_t id, const MeshData& mesh) override;
    void updateInstanceBatch(std::uint32_t id, const InstanceBatch& batch) override;
    void setTerrain(const TerrainDesc& terrain) override;
    void updateTerrain(const TerrainPatch& patch) override;
    void uploadTexture(std::uint32_t id, const TextureImage& image) override;
    void updateTexture(std::uint32_t id, const TexturePatch& patch) override;
    void loadUi(const UiDocument& document) override;
    void updateUi(std::span<const std::pair<std::string, UiValue>> values) override;
    void setMouseCursor(MouseCursor cursor) override;
    void beginFrame(const CameraView& camera) override;
    void submit(std::span<const DrawCommand> commands) override;
    void endFrame() override;
    [[nodiscard]] bool closeRequested() const override;
    void pollInput(InputState& input) override;

private:
    struct Impl; // guarda os tipos do NVRHI e da SDL fora do header público
    std::unique_ptr<Impl> impl_;
};

} // namespace lkz

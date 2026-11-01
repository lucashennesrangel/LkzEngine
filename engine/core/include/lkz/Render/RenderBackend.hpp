#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <string_view>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include "lkz/ECS/Components.hpp"
#include "lkz/Input/Input.hpp"
#include "lkz/Render/InstanceBatch.hpp"
#include "lkz/Render/MeshData.hpp"
#include "lkz/Render/TextureData.hpp"
#include "lkz/Terrain/TerrainDesc.hpp"
#include "lkz/UI/Ui.hpp"

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

    /// Troca a geometria de um id de mesh pela malha do jogo (World::uploadMesh).
    /// Chamado antes do frame, na thread de render. Backends sem janela ignoram.
    virtual void updateMesh(std::uint32_t id, const MeshData& mesh) {
        (void)id;
        (void)mesh;
    }

    /// Troca (ou remove, com `transforms` vazio) o lote de cópias paradas `id`
    /// (World::setInstanceBatch). Chamado antes do frame, na thread de render, depois
    /// das malhas. Backends sem janela ignoram.
    virtual void updateInstanceBatch(std::uint32_t id, const InstanceBatch& batch) {
        (void)id;
        (void)batch;
    }

    /// Troca (ou remove, com heightmap vazio) o terreno (World::setTerrain). Chamado antes
    /// do frame, na thread de render. Backends sem janela ignoram.
    virtual void setTerrain(const TerrainDesc& terrain) { (void)terrain; }
    /// Aplica uma mudança num retângulo do relevo (World::updateTerrain), depois de setTerrain.
    virtual void updateTerrain(const TerrainPatch& patch) { (void)patch; }

    /// Cria ou troca a textura `id` (World::uploadTexture). Chamado antes do frame, na
    /// thread de render, antes das malhas. Backends sem janela ignoram.
    virtual void uploadTexture(std::uint32_t id, const TextureImage& image) {
        (void)id;
        (void)image;
    }
    /// Muda um retângulo da textura `id` (World::updateTexture), depois do uploadTexture.
    virtual void updateTexture(std::uint32_t id, const TexturePatch& patch) {
        (void)id;
        (void)patch;
    }

    /// Carrega (ou troca) o documento da interface (World::showUi). Chamado antes do
    /// frame, na thread de render. Backends sem janela ignoram.
    virtual void loadUi(const UiDocument& document) { (void)document; }

    virtual void beginFrame(const CameraView& camera) = 0;
    virtual void submit(std::span<const DrawCommand> commands) = 0;
    /// Texto sobre a cena (linhas separadas por '\n'), entre submit() e endFrame().
    /// Só é chamado quando há texto. Backends sem janela ignoram.
    virtual void drawOverlay(std::string_view text) { (void)text; }
    /// Valores atuais da interface (todos), entre submit() e endFrame(). Só é chamado
    /// quando há valores.
    virtual void updateUi(std::span<const std::pair<std::string, UiValue>> values) { (void)values; }
    /// Forma do ponteiro do mouse (chamado todo frame: o backend troca só quando muda).
    virtual void setMouseCursor(MouseCursor cursor) { (void)cursor; }
    virtual void endFrame() = 0;

    /// true quando o usuário pediu para sair (ex.: fechou a janela). Consultado depois
    /// de cada frame; o World então encerra o loop.
    [[nodiscard]] virtual bool closeRequested() const { return false; }

    /// Atualiza `input` com o teclado e o mouse, depois de cada frame. Estado atual,
    /// posição e viewport são sobrescritos; eventos (`*Pressed`/`*Released`) e deltas
    /// devem ser ACUMULADOS (OR / soma): o World zera quando a lógica consome.
    /// Backends sem janela não fazem nada.
    virtual void pollInput(InputState& input) { (void)input; }
};

} // namespace lkz

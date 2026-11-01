#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <entt/entity/registry.hpp>

#include "lkz/Input/Input.hpp"
#include "lkz/Render/InstanceBatch.hpp"
#include "lkz/Render/MeshData.hpp"
#include "lkz/Render/TextureData.hpp"
#include "lkz/Render/RenderBackend.hpp"
#include "lkz/Render/RenderFrame.hpp"
#include "lkz/Terrain/TerrainDesc.hpp"
#include "lkz/UI/Ui.hpp"
#include "lkz/Scene/SceneNode.hpp"
#include "lkz/Systems/PhysicsSystem.hpp"
#include "lkz/Systems/RenderSystem.hpp"

namespace lkz {

class Camera;

struct WorldConfig {
    double fixedHz = 60.0;       // frequência do passo fixo de física/lógica
    double maxFrameRate = 240.0; // limite do laço principal (0 = sem limite)
    double maxFrameTime = 0.25;  // teto do delta real, em segundos (evita a "espiral da morte")
};

/// Dono do registry, do Scene Graph, dos sistemas e do game loop.
///
/// Ordem de um passo fixo:
///   1. callbacks de onFixedUpdate (sistemas de gameplay)
///   2. Entity::update            (onUpdate dos nós)
///   3. PhysicsSystem::update     (ECS em lote)
///   4. sync da cena              (só entidades que o observer marcou)
///   5. updateWorldTransform      (dirty flag + snapshots previous/current)
///
/// A cada volta do laço principal, um RenderFrame é empacotado e entregue à thread
/// de render (std::jthread) por um triple buffer protegido por mutex + condition_variable.
class World {
public:
    using FixedUpdateFn = std::function<void(World&, float)>;

    explicit World(std::unique_ptr<RenderBackend> backend, WorldConfig config = {});
    ~World();

    World(const World&) = delete;
    World& operator=(const World&) = delete;

    [[nodiscard]] entt::registry& registry() noexcept { return registry_; }
    [[nodiscard]] SceneNode& root() noexcept { return *root_; }

    void setActiveCamera(Camera* camera) noexcept { activeCamera_ = camera; }
    [[nodiscard]] Camera* activeCamera() const noexcept { return activeCamera_; }

    /// Teclado e mouse do passo fixo atual (válido dentro de onFixedUpdate e onUpdate).
    [[nodiscard]] const InputState& input() const noexcept { return input_; }

    /// Texto mostrado sobre a imagem (linhas separadas por '\n'), até ser trocado.
    /// Para status e ajuda de ferramentas; vazio esconde.
    void setOverlayText(std::string_view text) { overlay_.assign(text); }

    /// Mostra uma interface (documento RML/RCSS) por cima da cena, trocando a anterior
    /// (lkz/UI/Ui.hpp). Chega ao backend antes do próximo frame. Cliques chegam em
    /// input().uiEvents; o mouse sobre ela, em input().pointerOverUi.
    void showUi(UiDocument document);

    /// Muda um valor que a interface mostra (um dos nomes de UiDocument::values). Vale
    /// a partir do próximo frame publicado.
    void setUiValue(std::string_view name, UiValue value);

    /// Forma do ponteiro do mouse, até ser trocada. Sobre a interface, ela pode mostrar
    /// a dela. Backends sem janela ignoram.
    void setMouseCursor(MouseCursor cursor) { cursor_ = cursor; }

    /// Envia (ou substitui) a geometria de um id de mesh: o que tiver RenderComponent
    /// com esse id passa a ser desenhado com ela. Pode ser chamado a qualquer momento da
    /// thread lógica, inclusive antes de run(); a cópia chega ao backend antes do
    /// próximo frame. Para malhas procedurais (ruas, terreno...).
    void uploadMesh(std::uint32_t id, MeshData mesh);

    /// Envia (ou substitui) o lote de cópias paradas `id` de uma mesh: floresta, postes,
    /// prédios repetidos... Sem nó nem entidade por cópia; o backend descarta e escolhe
    /// o nível de detalhe por célula (lkz/Render/InstanceBatch.hpp). `transforms` vazio
    /// remove o lote. Mesma entrega do uploadMesh (thread lógica, antes do próximo frame).
    void setInstanceBatch(std::uint32_t id, InstanceBatch batch);

    /// Mostra (ou troca) o terreno: relevo + imagem, com nível de detalhe contínuo (CDLOD,
    /// lkz/Terrain). Heightmap vazio remove. Mesma entrega do uploadMesh. A lógica consulta
    /// a altura no próprio Heightmap (heightAt, raycast); a física usa o mesmo relevo.
    void setTerrain(TerrainDesc terrain);

    /// Muda um retângulo do relevo já mostrado (editor de terreno): só essa parte é
    /// reenviada. As mudanças chegam na ordem em que foram feitas.
    void updateTerrain(TerrainPatch patch);

    /// Envia (ou troca) a textura `id`, feita pelo jogo ou por uma ferramenta: as meshes com
    /// PrimitiveMesh::runtimeTexture == id passam a usá-la. Mesma entrega do uploadMesh.
    void uploadTexture(std::uint32_t id, TextureImage image);
    /// Muda um retângulo da textura `id` já enviada (pincel): só esse pedaço vai para a
    /// placa. As mudanças chegam na ordem em que foram feitas; `strokeEnd` refaz os mipmaps.
    void updateTexture(std::uint32_t id, TexturePatch patch);

    /// Registra um sistema de gameplay executado a cada passo fixo (antes dos nós).
    void onFixedUpdate(FixedUpdateFn fn);

    /// Roda o loop até requestStop() ou até maxDuration (se informado).
    void run(std::optional<std::chrono::duration<double>> maxDuration = std::nullopt);

    /// Pode ser chamado de qualquer thread.
    void requestStop() noexcept { stopRequested_.store(true, std::memory_order_relaxed); }

    [[nodiscard]] std::uint64_t tickCount() const noexcept { return tick_; }
    [[nodiscard]] float fixedDeltaTime() const noexcept { return static_cast<float>(1.0 / config_.fixedHz); }

private:
    using SceneObserver = entt::storage_for_t<entt::reactive>;

    void fixedUpdate(float dt);
    void consumeInput();
    void syncSceneFromEcs();
    void publishFrame(float alpha);
    void renderLoop(std::stop_token stopToken);

    WorldConfig config_;

    // A ordem de declaração importa: o registry precisa morrer depois da cena.
    entt::registry registry_;
    SceneObserver& sceneObserver_;
    PhysicsSystem physics_;
    RenderSystem renderSystem_;
    std::unique_ptr<SceneNode> root_;
    Camera* activeCamera_ = nullptr;
    std::vector<FixedUpdateFn> fixedCallbacks_;

    // Input: a render acumula em pendingInput_ (sob inputMutex_); cada passo fixo
    // copia para input_ e zera os deltas.
    InputState input_;
    InputState pendingInput_;
    std::mutex inputMutex_;

    std::string overlay_;
    std::vector<std::pair<std::string, UiValue>> uiValues_;
    MouseCursor cursor_ = MouseCursor::Default;

    // Malhas e lotes a enviar ao backend (thread lógica -> render), sob meshMutex_.
    std::vector<std::pair<std::uint32_t, MeshData>> pendingMeshes_;
    std::vector<std::pair<std::uint32_t, InstanceBatch>> pendingBatches_;
    std::optional<UiDocument> pendingUi_;
    std::optional<TerrainDesc> pendingTerrain_;
    std::vector<TerrainPatch> pendingTerrainPatches_;
    std::vector<std::pair<std::uint32_t, TextureImage>> pendingTextures_;
    std::vector<std::pair<std::uint32_t, TexturePatch>> pendingTexturePatches_;
    std::mutex meshMutex_;

    std::atomic<bool> stopRequested_{false};
    std::uint64_t tick_ = 0;

    // Triple buffer: a lógica escreve em writeIndex_, a render lê readIndex_, e
    // readyIndex_ guarda o frame pronto mais recente. Trocas só sob frameMutex_.
    std::unique_ptr<RenderBackend> backend_;
    std::array<RenderFrame, 3> frames_{};
    std::size_t writeIndex_ = 0;
    std::size_t readyIndex_ = 1;
    std::size_t readIndex_ = 2;
    bool frameReady_ = false;
    std::mutex frameMutex_;
    std::condition_variable_any frameCv_;
    std::jthread renderThread_; // por último: é parado antes de qualquer outro membro morrer
};

} // namespace lkz

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
#include <thread>
#include <vector>

#include <entt/entity/registry.hpp>

#include "lkz/Render/RenderBackend.hpp"
#include "lkz/Render/RenderFrame.hpp"
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

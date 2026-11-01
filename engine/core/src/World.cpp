#include "lkz/World.hpp"

#include <algorithm>

#include <spdlog/spdlog.h>
#include <tracy/Tracy.hpp>

#include "lkz/ECS/Components.hpp"
#include "lkz/Scene/Camera.hpp"

namespace lkz {

using namespace entt::literals;

World::World(std::unique_ptr<RenderBackend> backend, WorldConfig config)
    : config_{config}
    , sceneObserver_{registry_.storage<entt::reactive>("lkz::scene_observer"_hs)}
    , physics_{registry_}
    , renderSystem_{registry_}
    , backend_{std::move(backend)} {
    // Observer da cena: guarda as entidades cujo transform foi criado ou substituído.
    // Escritas diretas por referência (hot path da física) não disparam nada.
    sceneObserver_.on_construct<PositionComponent>()
        .on_update<PositionComponent>()
        .on_update<RotationComponent>()
        .on_update<ScaleComponent>();

    root_ = std::make_unique<SceneNode>("Root", registry_);

    if (backend_ == nullptr) {
        spdlog::warn("World criado sem RenderBackend: os frames serão descartados");
    }
}

World::~World() {
    if (renderThread_.joinable()) {
        renderThread_.request_stop();
        renderThread_.join();
    }
    root_.reset();
    // O reactive storage não se desconecta sozinho dos pools observados.
    sceneObserver_.reset();
}

void World::uploadMesh(std::uint32_t id, MeshData mesh) {
    const std::scoped_lock lock{meshMutex_};
    pendingMeshes_.emplace_back(id, std::move(mesh));
}

void World::showUi(UiDocument document) {
    for (const auto& [name, value] : document.values) {
        setUiValue(name, value);
    }
    const std::scoped_lock lock{meshMutex_};
    pendingUi_ = std::move(document);
}

void World::setUiValue(std::string_view name, UiValue value) {
    const auto it = std::find_if(uiValues_.begin(), uiValues_.end(), [&](const auto& entry) { return entry.first == name; });
    if (it != uiValues_.end()) {
        it->second = std::move(value);
    } else {
        uiValues_.emplace_back(std::string{name}, std::move(value));
    }
}

void World::setTerrain(TerrainDesc terrain) {
    const std::scoped_lock lock{meshMutex_};
    pendingTerrain_ = std::move(terrain);
}

void World::updateTerrain(TerrainPatch patch) {
    const std::scoped_lock lock{meshMutex_};
    pendingTerrainPatches_.push_back(std::move(patch));
}

void World::uploadTexture(std::uint32_t id, TextureImage image) {
    const std::scoped_lock lock{meshMutex_};
    pendingTextures_.emplace_back(id, std::move(image));
}

void World::updateTexture(std::uint32_t id, TexturePatch patch) {
    const std::scoped_lock lock{meshMutex_};
    pendingTexturePatches_.emplace_back(id, std::move(patch));
}

void World::setInstanceBatch(std::uint32_t id, InstanceBatch batch) {
    const std::scoped_lock lock{meshMutex_};
    pendingBatches_.emplace_back(id, std::move(batch));
}

void World::onFixedUpdate(FixedUpdateFn fn) {
    fixedCallbacks_.push_back(std::move(fn));
}

void World::run(std::optional<std::chrono::duration<double>> maxDuration) {
    using Clock = std::chrono::steady_clock;
    using Seconds = std::chrono::duration<double>;

    const Seconds fixedDt{1.0 / config_.fixedHz};
    const Seconds maxFrameTime{config_.maxFrameTime};
    const auto minFrameTime = config_.maxFrameRate > 0.0
                                  ? std::chrono::duration_cast<Clock::duration>(Seconds{1.0 / config_.maxFrameRate})
                                  : Clock::duration::zero();

    stopRequested_.store(false, std::memory_order_relaxed);
    std::uint64_t framesPublished = 0;
    const std::uint64_t firstTick = tick_;

    spdlog::info("Loop iniciado: passo fixo de {:.3f} ms, {} entidades com PositionComponent",
                 fixedDt.count() * 1000.0, registry_.storage<PositionComponent>().size());

    renderThread_ = std::jthread{[this](std::stop_token stopToken) { renderLoop(std::move(stopToken)); }};

    const auto start = Clock::now();
    auto previous = start;
    Seconds accumulator{0.0};

    while (!stopRequested_.load(std::memory_order_relaxed)) {
        const auto frameStart = Clock::now();
        if (maxDuration && frameStart - start >= *maxDuration) {
            break;
        }

        Seconds frameTime = frameStart - previous;
        previous = frameStart;
        if (frameTime > maxFrameTime) {
            frameTime = maxFrameTime;
        }

        accumulator += frameTime;
        while (accumulator >= fixedDt) {
            fixedUpdate(static_cast<float>(fixedDt.count()));
            accumulator -= fixedDt;
        }

        // Quanto do próximo passo já "passou": 0 = estado anterior, 1 = estado atual.
        const auto alpha = static_cast<float>(accumulator / fixedDt);
        publishFrame(alpha);
        ++framesPublished;
        FrameMarkNamed("Logic");

        if (minFrameTime > Clock::duration::zero()) {
            std::this_thread::sleep_until(frameStart + minFrameTime);
        }
    }

    renderThread_.request_stop();
    renderThread_.join();

    const Seconds elapsed = Clock::now() - start;
    spdlog::info("Loop finalizado: {:.2f} s, {} ticks, {} frames publicados", elapsed.count(), tick_ - firstTick,
                 framesPublished);
}

void World::fixedUpdate(float dt) {
    ZoneScoped;

    consumeInput();
    for (auto& callback : fixedCallbacks_) {
        callback(*this, dt);
    }
    root_->update(dt);
    physics_.update(dt);
    syncSceneFromEcs();
    root_->updateWorldTransform();
    ++tick_;
}

void World::consumeInput() {
    const std::scoped_lock lock{inputMutex_};
    input_ = pendingInput_;
    pendingInput_.clearEvents();
}

void World::syncSceneFromEcs() {
    ZoneScoped;

    for (const auto entity : sceneObserver_) {
        const auto* link = registry_.try_get<SceneLinkComponent>(entity);
        if (link == nullptr) {
            continue; // entidade pura do ECS: não pertence ao Scene Graph
        }
        if (link->node == nullptr || link->node->handle() != entity) {
            spdlog::error("Observer da cena: SceneLinkComponent inválido na entidade {}", entt::to_integral(entity));
            continue;
        }
        link->node->syncFromEcs();
    }
    sceneObserver_.clear();
}

void World::publishFrame(float alpha) {
    ZoneScoped;

    renderSystem_.pack(frames_[writeIndex_], activeCamera_, alpha, tick_);
    frames_[writeIndex_].overlay = overlay_; // assign reaproveita a capacidade
    frames_[writeIndex_].ui = uiValues_;
    frames_[writeIndex_].cursor = cursor_;
    {
        const std::scoped_lock lock{frameMutex_};
        std::swap(writeIndex_, readyIndex_);
        frameReady_ = true;
    }
    frameCv_.notify_one();
}

void World::renderLoop(std::stop_token stopToken) {
    tracy::SetThreadName("Render");

    if (backend_ != nullptr && !backend_->initialize()) {
        spdlog::error("Falha ao inicializar o RenderBackend; encerrando o loop");
        requestStop();
        return;
    }

    for (;;) {
        {
            std::unique_lock lock{frameMutex_};
            if (!frameCv_.wait(lock, stopToken, [this] { return frameReady_; })) {
                break; // parada solicitada sem frame pendente
            }
            std::swap(readIndex_, readyIndex_);
            frameReady_ = false;
        }

        if (backend_ != nullptr) {
            std::vector<std::pair<std::uint32_t, MeshData>> meshes;
            std::vector<std::pair<std::uint32_t, InstanceBatch>> batches;
            std::optional<UiDocument> ui;
            std::optional<TerrainDesc> terrain;
            std::vector<TerrainPatch> terrainPatches;
            std::vector<std::pair<std::uint32_t, TextureImage>> textures;
            std::vector<std::pair<std::uint32_t, TexturePatch>> texturePatches;
            {
                const std::scoped_lock lock{meshMutex_};
                meshes.swap(pendingMeshes_);
                batches.swap(pendingBatches_);
                ui.swap(pendingUi_);
                terrain.swap(pendingTerrain_);
                terrainPatches.swap(pendingTerrainPatches_);
                textures.swap(pendingTextures_);
                texturePatches.swap(pendingTexturePatches_);
            }
            for (const auto& [id, image] : textures) {
                backend_->uploadTexture(id, image);
            }
            for (const auto& [id, patch] : texturePatches) {
                backend_->updateTexture(id, patch);
            }
            if (terrain) {
                backend_->setTerrain(*terrain);
            }
            for (const auto& patch : terrainPatches) {
                backend_->updateTerrain(patch);
            }
            if (ui) {
                backend_->loadUi(*ui);
            }
            for (const auto& [id, mesh] : meshes) {
                backend_->updateMesh(id, mesh);
            }
            for (const auto& [id, batch] : batches) {
                backend_->updateInstanceBatch(id, batch);
            }
            renderSystem_.render(frames_[readIndex_], *backend_);
            {
                const std::scoped_lock lock{inputMutex_};
                backend_->pollInput(pendingInput_);
            }
            if (backend_->closeRequested()) {
                requestStop();
            }
        }
        FrameMark;

        if (stopToken.stop_requested()) {
            break;
        }
    }

    if (backend_ != nullptr) {
        backend_->shutdown();
    }
}

} // namespace lkz

// Sandbox: cena mínima para validar o núcleo.
//   Root
//   ├── Player   (gira em círculo; lógica em onUpdate)
//   │   └── Weapon (offset fixo: herda o movimento do Player pela hierarquia)
//   └── Camera
// + N entidades puras no ECS movidas pelo PhysicsSystem.
//
// Uso: sandbox [segundos] [entidades]   (padrão: 2 s, 5000 entidades)

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <random>

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>
#include <spdlog/spdlog.h>
#include <tracy/Tracy.hpp>

#include <lkz/Core/Log.hpp>
#include <lkz/ECS/Components.hpp>
#include <lkz/Scene/Camera.hpp>
#include <lkz/World.hpp>
#include <lkz/backends/NullRenderBackend.hpp>

namespace {

class Player final : public lkz::SceneNode {
public:
    using SceneNode::SceneNode;

protected:
    void onUpdate(float dt) override {
        time_ += dt;
        setPosition({std::cos(time_) * radius, 0.0f, std::sin(time_) * radius});
        setRotation(glm::angleAxis(-time_, glm::vec3{0.0f, 1.0f, 0.0f}));
    }

private:
    static constexpr float radius = 5.0f;
    float time_ = 0.0f;
};

void stressTest(lkz::World& world, std::size_t count) {
    ZoneScoped;

    auto& registry = world.registry();
    std::mt19937 rng{1337};
    std::uniform_real_distribution<float> position{-50.0f, 50.0f};
    std::uniform_real_distribution<float> velocity{-5.0f, 5.0f};
    std::uniform_real_distribution<float> color{0.2f, 1.0f};

    for (std::size_t i = 0; i < count; ++i) {
        const auto entity = registry.create();
        const glm::vec3 start{position(rng), position(rng), position(rng)};
        registry.emplace<lkz::PositionComponent>(entity, start, start);
        registry.emplace<lkz::VelocityComponent>(entity, glm::vec3{velocity(rng), velocity(rng), velocity(rng)});
        registry.emplace<lkz::RenderComponent>(entity, 0u, 0u, glm::vec4{color(rng), color(rng), color(rng), 1.0f});
    }

    spdlog::info("stressTest: {} entidades puras criadas no ECS", count);
}

} // namespace

int main(int argc, char** argv) {
    lkz::initLogging();

    const double seconds = argc > 1 ? std::atof(argv[1]) : 2.0;
    const auto entityCount = argc > 2 ? static_cast<std::size_t>(std::atoll(argv[2])) : std::size_t{5000};

    lkz::World world{std::make_unique<lkz::NullRenderBackend>()};
    auto& registry = world.registry();
    auto& root = world.root();

    auto& player = root.emplaceChild<Player>("Player", registry);
    registry.emplace<lkz::RenderComponent>(player.handle(), 1u, 0u, glm::vec4{0.2f, 0.6f, 1.0f, 1.0f});

    auto& weapon = player.emplaceChild<lkz::SceneNode>("Weapon", registry);
    weapon.setPosition({0.6f, 1.2f, 0.0f});
    registry.emplace<lkz::RenderComponent>(weapon.handle(), 2u, 0u, glm::vec4{0.9f, 0.9f, 0.2f, 1.0f});

    auto& camera = root.emplaceChild<lkz::Camera>("Camera", registry);
    const glm::vec3 eye{0.0f, 12.0f, 20.0f};
    camera.setPosition(eye);
    camera.setRotation(glm::quatLookAt(glm::normalize(glm::vec3{0.0f} - eye), glm::vec3{0.0f, 1.0f, 0.0f}));
    world.setActiveCamera(&camera);

    stressTest(world, entityCount);

    world.run(std::chrono::duration<double>{seconds});

    // A posição de mundo da arma deve acompanhar o Player (cascata pela hierarquia).
    const glm::vec3 playerWorld{player.worldMatrix()[3]};
    const glm::vec3 weaponWorld{weapon.worldMatrix()[3]};
    spdlog::info("Player em ({:.2f}, {:.2f}, {:.2f}) | Weapon em ({:.2f}, {:.2f}, {:.2f})", playerWorld.x,
                 playerWorld.y, playerWorld.z, weaponWorld.x, weaponWorld.y, weaponWorld.z);
    return EXIT_SUCCESS;
}

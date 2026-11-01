// Physics: demo do lkz::physics (Jolt) sobre o terreno da engine (World::setTerrain).
//   - Relevo procedural de 2 x 2 km (morros), desenhado em CDLOD; o mesmo Heightmap é o
//     heightfield da física.
//   - 200 caixas caindo (entidades com PhysicsBodyComponent, sincronizadas com a cena).
//   - Um carro: W/S ou setas aceleram e freiam, A/D ou setas viram, Espaço é o freio de
//     mão. A câmera segue.
//   - F3: níveis de detalhe do terreno coloridos.
// Uso: physics [segundos]   (0 = até fechar a janela)

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <spdlog/spdlog.h>

#include <lkz/Core/Log.hpp>
#include <lkz/ECS/Components.hpp>
#include <lkz/Physics/PhysicsWorld.hpp>
#include <lkz/Scene/Camera.hpp>
#include <lkz/Terrain/Heightmap.hpp>
#include <lkz/World.hpp>
#include <lkz/backends/RaylibRenderBackend.hpp>

namespace {

namespace mesh {
constexpr std::uint32_t box = 1;
constexpr std::uint32_t wheel = 2;
constexpr std::uint32_t car = 3;
} // namespace mesh

/// Morros suaves, com uma área plana no começo da estrada.
lkz::Heightmap makeTerrain() {
    lkz::Heightmap map = lkz::Heightmap::covering({2048.0f, 2048.0f}, 4.0f);
    for (int r = 0; r < map.rows(); ++r) {
        for (int c = 0; c < map.columns(); ++c) {
            const glm::vec2 p = map.origin() + glm::vec2{static_cast<float>(c), static_cast<float>(r)} * map.spacing();
            float h = 18.0f * std::sin(p.x * 0.004f) * std::cos(p.y * 0.0035f) + 6.0f * std::sin(p.x * 0.013f + 1.0f) +
                      4.0f * std::cos(p.y * 0.017f);
            const float flat = std::exp(-glm::dot(p, p) / (2.0f * 120.0f * 120.0f)); // 1 no centro
            h = glm::mix(h, 0.0f, flat);
            map.set(c, r, h);
        }
    }
    return map;
}

glm::quat lookRotation(glm::vec3 from, glm::vec3 to) {
    return glm::quatLookAt(glm::normalize(to - from), glm::vec3{0.0f, 1.0f, 0.0f});
}

} // namespace

int main(int argc, char** argv) {
    lkz::initLogging();
    const double seconds = argc > 1 ? std::atof(argv[1]) : 0.0;

    lkz::VehicleDesc carDesc;
    carDesc.position = {0.0f, 2.0f, 0.0f};

    lkz::RaylibConfig config;
    config.title = "Lkz Engine - physics";
    config.clearColor = {0.55f, 0.7f, 0.85f, 1.0f};
    config.meshes = {
        {mesh::box, {lkz::PrimitiveShape::Box, glm::vec3{1.0f}}},
        {mesh::wheel, {lkz::PrimitiveShape::Cylinder, {carDesc.wheelRadius * 2.0f, carDesc.wheelWidth, carDesc.wheelRadius * 2.0f}}},
        {mesh::car, {lkz::PrimitiveShape::Box, carDesc.halfExtents * 2.0f}},
    };
    lkz::World world{std::make_unique<lkz::RaylibRenderBackend>(std::move(config))};
    auto& registry = world.registry();
    auto& root = world.root();

    lkz::Heightmap terrain = makeTerrain();
    lkz::PhysicsWorld physics;
    physics.addHeightfield(terrain);
    lkz::TerrainDesc terrainDesc;
    terrainDesc.heightmap = terrain;
    terrainDesc.color = {0.42f, 0.58f, 0.33f, 1.0f};
    world.setTerrain(std::move(terrainDesc));

    // Caixas: cada uma é um nó da cena ligado a um corpo.
    std::mt19937 rng{7};
    std::uniform_real_distribution<float> spread{-60.0f, 60.0f};
    std::uniform_real_distribution<float> size{0.4f, 1.6f};
    std::uniform_real_distribution<float> tint{0.3f, 1.0f};
    for (int i = 0; i < 200; ++i) {
        const glm::vec3 half{size(rng) * 0.5f, size(rng) * 0.5f, size(rng) * 0.5f};
        const glm::vec3 position{spread(rng), 10.0f + static_cast<float>(i) * 0.6f, 30.0f + spread(rng)};
        auto& node = root.emplaceChild<lkz::SceneNode>("Caixa " + std::to_string(i), registry);
        node.setScale(half * 2.0f);
        registry.emplace<lkz::RenderComponent>(node.handle(), mesh::box, 0u, glm::vec4{tint(rng), tint(rng), tint(rng), 1.0f});
        registry.emplace<lkz::PhysicsBodyComponent>(node.handle(), physics.addBody({lkz::BoxShape{half}, position}));
    }

    // Carro: carroceria ligada ao corpo; rodas posicionadas a cada passo pelo estado do veículo.
    const std::uint32_t car = physics.addVehicle(carDesc);
    auto& carNode = root.emplaceChild<lkz::SceneNode>("Carro", registry);
    registry.emplace<lkz::RenderComponent>(carNode.handle(), mesh::car, 0u, glm::vec4{0.85f, 0.15f, 0.12f, 1.0f});
    registry.emplace<lkz::PhysicsBodyComponent>(carNode.handle(), physics.vehicleBody(car));
    std::vector<lkz::SceneNode*> wheels;
    for (int i = 0; i < 4; ++i) {
        auto& wheel = root.emplaceChild<lkz::SceneNode>("Roda " + std::to_string(i), registry);
        registry.emplace<lkz::RenderComponent>(wheel.handle(), mesh::wheel, 0u, glm::vec4{0.12f, 0.12f, 0.13f, 1.0f});
        wheels.push_back(&wheel);
    }

    auto& camera = root.emplaceChild<lkz::Camera>("Câmera", registry);
    camera.projection.farPlane = 5000.0f;
    world.setActiveCamera(&camera);

    world.onFixedUpdate([&](lkz::World& w, float dt) {
        using lkz::Key;
        const auto& input = w.input();
        lkz::VehicleInput drive;
        const bool up = input.down(Key::Up) || input.down(Key::W);
        const bool down = input.down(Key::Down) || input.down(Key::S);
        const bool left = input.down(Key::Left) || input.down(Key::A);
        const bool right = input.down(Key::Right) || input.down(Key::D);
        drive.forward = up ? 1.0f : (down ? -1.0f : 0.0f);
        drive.right = right ? 1.0f : (left ? -1.0f : 0.0f);
        drive.handBrake = input.down(Key::Space) ? 1.0f : 0.0f;
        physics.setVehicleInput(car, drive);
        physics.step(dt);
        physics.syncToScene(w.registry());

        const lkz::VehicleState state = physics.vehicleState(car);
        for (std::size_t i = 0; i < wheels.size(); ++i) {
            wheels[i]->setPosition(glm::vec3{state.wheels[i][3]});
            wheels[i]->setRotation(glm::quat_cast(glm::mat3{state.wheels[i]}));
        }
        // Câmera atrás e acima do carro.
        const glm::vec3 forward = state.body.rotation * glm::vec3{0.0f, 0.0f, 1.0f};
        const glm::vec3 flatForward = glm::normalize(glm::vec3{forward.x, 0.0f, forward.z} + glm::vec3{0.0f, 0.0f, 1e-4f});
        const glm::vec3 eye = state.body.position - flatForward * 11.0f + glm::vec3{0.0f, 4.5f, 0.0f};
        camera.setPosition(eye);
        camera.setRotation(lookRotation(eye, state.body.position + glm::vec3{0.0f, 1.0f, 0.0f}));
    });

    world.run(seconds > 0.0 ? std::optional{std::chrono::duration<double>{seconds}} : std::nullopt);
    const lkz::VehicleState state = physics.vehicleState(car);
    spdlog::info("physics: carro em ({:.1f}, {:.1f}, {:.1f}), {:.1f} m/s", state.body.position.x, state.body.position.y,
                 state.body.position.z, state.speed);
    return EXIT_SUCCESS;
}

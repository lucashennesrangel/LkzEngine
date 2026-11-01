// Window: demo do lkz::renderer_raylib.
//   Root
//   ├── Ground    (Plane com a imagem assets/grid.png: faixa vermelha = -Z, azul = -X)
//   ├── Orbiter   (esfera em órbita; lógica em onUpdate)
//   │   └── Moon  (cilindro preso à esfera pela hierarquia)
//   ├── Pad       (hexágono com a imagem do chão: malha enviada por World::uploadMesh)
//   ├── Patch     (placa com a imagem do chão em hex-tiling: pedaços girados, sem grade)
//   ├── House 1..3 (modelo assets/house.glb: paredes e telhado, um material de cor cada)
//   └── Camera
// + N cubos puros do ECS quicando dentro de uma caixa.
// + uma vila de 1600 casas num lote parado (World::setInstanceBatch), sem nó nem
//   entidade por casa: de longe viram impostores; fora da câmera, nem são desenhadas.
// A esfera tem um LOD gerado (um quarto dos triângulos) para quando fica pequena.
// Interface (World::showUi, assets/ui/demo.rml): estatísticas do frame, um botão do jogo
// (pausar a órbita) e um da engine (LOD colorido). F8 abre o depurador da RmlUi.
//
// Fecha com o botão da janela ou ESC.
// Uso: window [entidades] [segundos]   (padrão: 10000 entidades, 0 = até fechar a janela)

#include <chrono>
#include <filesystem>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <string_view>
#include <vector>
#include <memory>
#include <optional>
#include <random>
#include <string>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>
#include <spdlog/spdlog.h>

#include <lkz/Core/Log.hpp>
#include <lkz/ECS/Components.hpp>
#include <lkz/Scene/Camera.hpp>
#include <lkz/World.hpp>
#include <lkz/backends/RaylibRenderBackend.hpp>
#if defined(LKZ_WITH_NVRHI)
#include <lkz/backends/NvrhiRenderBackend.hpp>
#endif

namespace {

namespace mesh {
constexpr std::uint32_t cube = 1;
constexpr std::uint32_t sphere = 2;
constexpr std::uint32_t cylinder = 3;
constexpr std::uint32_t ground = 4;
constexpr std::uint32_t pad = 5; // malha montada aqui mesmo (World::uploadMesh)
constexpr std::uint32_t patch = 6; // TextureTiling::Hex
constexpr std::uint32_t house = 7; // PrimitiveShape::Model (make_house_glb.py)
} // namespace mesh

constexpr float boxHalfSize = 30.0f;

class Orbiter final : public lkz::SceneNode {
public:
    using SceneNode::SceneNode;

    bool paused = false;

protected:
    void onUpdate(float dt) override {
        if (paused) {
            return;
        }
        time_ += dt;
        setPosition({std::cos(time_) * radius, 6.0f, std::sin(time_) * radius});
        setRotation(glm::angleAxis(-time_ * 2.0f, glm::vec3{0.0f, 1.0f, 0.0f}));
    }

private:
    static constexpr float radius = 15.0f;
    float time_ = 0.0f;
};

/// Rebate as entidades nas paredes da caixa (o PhysicsSystem só integra a velocidade).
void bounce(lkz::World& world, float /*dt*/) {
    world.registry().view<lkz::VelocityComponent, const lkz::PositionComponent>().each(
        [](lkz::VelocityComponent& velocity, const lkz::PositionComponent& position) {
            for (int axis = 0; axis < 3; ++axis) {
                const float low = axis == 1 ? 0.5f : -boxHalfSize;
                if ((position.value[axis] < low && velocity.value[axis] < 0.0f) ||
                    (position.value[axis] > boxHalfSize && velocity.value[axis] > 0.0f)) {
                    velocity.value[axis] = -velocity.value[axis];
                }
            }
        });
}

} // namespace

int main(int argc, char** argv) {
    lkz::initLogging();

    // window [cópias] [segundos] [--d3d12 | --vulkan] [--debug]
    std::vector<std::string_view> numbers;
    std::string_view backendName = "raylib";
    bool debug = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--debug") {
            debug = true;
        } else if (arg.starts_with("--")) {
            backendName = arg.substr(2);
        } else {
            numbers.push_back(arg);
        }
    }
    const auto entityCount = !numbers.empty() ? static_cast<std::size_t>(std::atoll(numbers[0].data())) : std::size_t{10000};
    const double seconds = numbers.size() > 1 ? std::atof(numbers[1].data()) : 0.0;

    // As imagens e o modelo são copiados para junto do executável pelo CMake.
    const auto assets = std::filesystem::absolute(argv[0]).parent_path() / "assets";
    const auto gridImage = assets / "grid.png";

    lkz::PrimitiveMesh house{lkz::PrimitiveShape::Model};
    house.model = (assets / "house.glb").string();
    house.impostor = {true, 0.08f, 8, 64}; // abaixo de 8% da tela: quadrado com fotos

    lkz::PrimitiveMesh sphere{lkz::PrimitiveShape::Sphere, glm::vec3{3.0f}};
    sphere.lods = {{0.2f, 0.25f}}; // abaixo de 20% da tela: um quarto dos triângulos

    lkz::RaylibConfig config;
    config.title = "Lkz Engine - window";
    config.meshes = {
        {mesh::cube, {lkz::PrimitiveShape::Box, glm::vec3{0.6f}}},
        {mesh::sphere, sphere},
        {mesh::cylinder, {lkz::PrimitiveShape::Cylinder, {0.8f, 3.0f, 0.8f}}},
        {mesh::ground, {lkz::PrimitiveShape::Plane, glm::vec3{1.0f}, glm::vec3{0.0f}, gridImage.string()}},
        {mesh::pad, {lkz::PrimitiveShape::Custom, glm::vec3{1.0f}, glm::vec3{0.0f}, gridImage.string()}},
        {mesh::patch, {lkz::PrimitiveShape::Plane, glm::vec3{1.0f}, glm::vec3{0.0f}, gridImage.string(),
                       lkz::TextureTiling::Hex}},
        {mesh::house, house},
    };

    std::unique_ptr<lkz::RenderBackend> backend;
#if defined(LKZ_WITH_NVRHI)
    if (const auto api = lkz::parseGraphicsApi(backendName)) {
        config.title += std::string{" ("} + lkz::graphicsApiName(*api) + ")";
        backend = std::make_unique<lkz::NvrhiRenderBackend>(std::move(config), *api, debug);
    }
#endif
    if (!backend) {
        backend = std::make_unique<lkz::RaylibRenderBackend>(std::move(config));
    }
    lkz::World world{std::move(backend)};
    auto& registry = world.registry();
    auto& root = world.root();

    auto& ground = root.emplaceChild<lkz::SceneNode>("Ground", registry);
    ground.setScale({boxHalfSize * 2.0f, 1.0f, boxHalfSize * 2.0f});
    registry.emplace<lkz::RenderComponent>(ground.handle(), mesh::ground, 0u, glm::vec4{1.0f});

    // Malha procedural: hexágono em leque, um pouco acima do chão.
    lkz::MeshData hexagon;
    hexagon.positions.push_back({0.0f, 0.05f, 0.0f});
    hexagon.uvs.push_back({0.5f, 0.5f});
    for (int i = 0; i < 6; ++i) {
        const float angle = glm::radians(60.0f * static_cast<float>(i));
        hexagon.positions.push_back({std::cos(angle) * 8.0f, 0.05f, std::sin(angle) * 8.0f});
        hexagon.uvs.push_back({0.5f + std::cos(angle) * 0.5f, 0.5f + std::sin(angle) * 0.5f});
    }
    for (std::uint32_t i = 0; i < 6; ++i) {
        // Anti-horário visto de cima (+Y): centro, próximo, atual.
        hexagon.indices.insert(hexagon.indices.end(), {0u, 1u + (i + 1) % 6, 1u + i});
    }
    world.uploadMesh(mesh::pad, std::move(hexagon));
    auto& pad = root.emplaceChild<lkz::SceneNode>("Pad", registry);
    pad.setPosition({-15.0f, 0.0f, 15.0f});
    registry.emplace<lkz::RenderComponent>(pad.handle(), mesh::pad, 0u, glm::vec4{1.0f, 0.8f, 0.4f, 1.0f});

    // A mesma imagem repetida 6 x 6 vezes: em grade, as faixas coloridas formariam um
    // xadrez regular; em hex-tiling, cada pedaço sai deslocado e girado.
    auto& patch = root.emplaceChild<lkz::SceneNode>("Patch", registry);
    patch.setPosition({15.0f, 0.05f, 15.0f});
    patch.setScale({16.0f, 1.0f, 16.0f});
    registry.emplace<lkz::RenderComponent>(patch.handle(), mesh::patch, 0u, glm::vec4{1.0f},
                                           glm::vec4{6.0f, 6.0f, 0.0f, 0.0f});

    // O mesmo modelo três vezes: um draw call por parte, com as três instâncias.
    for (int i = 0; i < 3; ++i) {
        auto& node = root.emplaceChild<lkz::SceneNode>("House " + std::to_string(i + 1), registry);
        node.setPosition({-20.0f + 7.0f * static_cast<float>(i), 0.0f, -18.0f});
        node.setRotation(glm::angleAxis(glm::radians(20.0f * static_cast<float>(i)), glm::vec3{0.0f, 1.0f, 0.0f}));
        registry.emplace<lkz::RenderComponent>(node.handle(), mesh::house, 0u, glm::vec4{1.0f});
    }

    // Vila atrás da caixa: 40 x 40 casas num lote só.
    lkz::InstanceBatch village{mesh::house};
    for (int row = 0; row < 40; ++row) {
        for (int column = 0; column < 40; ++column) {
            const glm::vec3 position{-200.0f + 10.0f * static_cast<float>(column), 0.0f,
                                     -45.0f - 10.0f * static_cast<float>(row)};
            const float angle = glm::radians(static_cast<float>((row * 37 + column * 11) % 360));
            village.transforms.push_back(glm::translate(glm::mat4{1.0f}, position) *
                                         glm::mat4_cast(glm::angleAxis(angle, glm::vec3{0.0f, 1.0f, 0.0f})));
        }
    }
    world.setInstanceBatch(1, std::move(village));

    auto& orbiter = root.emplaceChild<Orbiter>("Orbiter", registry);
    registry.emplace<lkz::RenderComponent>(orbiter.handle(), mesh::sphere, 0u, glm::vec4{1.0f, 0.55f, 0.1f, 1.0f});

    auto& moon = orbiter.emplaceChild<lkz::SceneNode>("Moon", registry);
    moon.setPosition({4.0f, 0.0f, 0.0f});
    registry.emplace<lkz::RenderComponent>(moon.handle(), mesh::cylinder, 0u, glm::vec4{0.9f, 0.9f, 0.95f, 1.0f});

    auto& camera = root.emplaceChild<lkz::Camera>("Camera", registry);
    const glm::vec3 eye{0.0f, 40.0f, 60.0f};
    camera.setPosition(eye);
    camera.setRotation(glm::quatLookAt(glm::normalize(glm::vec3{0.0f, 5.0f, 0.0f} - eye), glm::vec3{0.0f, 1.0f, 0.0f}));
    world.setActiveCamera(&camera);

    std::mt19937 rng{1337};
    std::uniform_real_distribution<float> position{-boxHalfSize, boxHalfSize};
    std::uniform_real_distribution<float> velocity{-6.0f, 6.0f};
    std::uniform_real_distribution<float> color{0.2f, 1.0f};
    for (std::size_t i = 0; i < entityCount; ++i) {
        const auto entity = registry.create();
        const glm::vec3 start{position(rng), std::abs(position(rng)) + 0.5f, position(rng)};
        registry.emplace<lkz::PositionComponent>(entity, start, start);
        registry.emplace<lkz::VelocityComponent>(entity, glm::vec3{velocity(rng), velocity(rng), velocity(rng)});
        registry.emplace<lkz::RenderComponent>(entity, mesh::cube, 0u, glm::vec4{color(rng), color(rng), color(rng), 1.0f});
    }

    world.onFixedUpdate(bounce);

    lkz::UiDocument ui;
    ui.path = (assets / "ui" / "demo.rml").string();
    ui.fonts = {(assets / "ui" / "Barlow-Regular.ttf").string(), (assets / "ui" / "Barlow-SemiBold.ttf").string()};
    ui.values = {{"cliques", 0}, {"pausada", false}};
    world.showUi(std::move(ui));
    int clicks = 0;
    world.onFixedUpdate([&orbiter, &clicks](lkz::World& w, float /*dt*/) {
        for (const auto& event : w.input().uiEvents) {
            if (event.action == "orbita") {
                orbiter.paused = !orbiter.paused;
                w.setUiValue("pausada", orbiter.paused);
                w.setUiValue("cliques", ++clicks);
            }
        }
    });
    world.run(seconds > 0.0 ? std::optional{std::chrono::duration<double>{seconds}} : std::nullopt);
    spdlog::info("window: {} ticks", world.tickCount());
    return EXIT_SUCCESS;
}

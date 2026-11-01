// texture_paint: textura feita em tempo real (World::uploadTexture / updateTexture). Um
// cubo usa a textura 7 (PrimitiveMesh::runtimeTexture); a cada passo da lógica um pincel
// redondo pinta nela, andando em círculo, e a cada segundo o traço termina (os mipmaps
// são refeitos).
//
// Uso: texture_paint [segundos] [--d3d12 | --vulkan]

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <lkz/Core/Log.hpp>
#include <lkz/Scene/Camera.hpp>
#include <lkz/World.hpp>
#include <lkz/backends/RaylibRenderBackend.hpp>
#if defined(LKZ_WITH_NVRHI)
#include <lkz/backends/NvrhiRenderBackend.hpp>
#endif

namespace {

constexpr std::uint32_t cubeMesh = 1;
constexpr std::uint32_t canvas = 7;
constexpr int size = 256;
constexpr int brush = 24;

lkz::TextureImage grid() {
    lkz::TextureImage image;
    image.width = size;
    image.height = size;
    image.rgba.resize(static_cast<std::size_t>(size * size * 4));
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const auto i = static_cast<std::size_t>((y * size + x) * 4);
            const std::uint8_t v = (x % 32 == 0 || y % 32 == 0) ? 90 : 230;
            image.rgba[i] = v;
            image.rgba[i + 1] = v;
            image.rgba[i + 2] = v;
            image.rgba[i + 3] = 255;
        }
    }
    return image;
}

} // namespace

int main(int argc, char** argv) {
    lkz::initLogging();
    double seconds = 0.0;
    std::string_view backendName;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg.starts_with("--")) {
            backendName = arg.substr(2);
        } else {
            seconds = std::atof(argv[i]);
        }
    }

    lkz::RenderConfig config;
    config.title = "Lkz Engine - texture_paint";
    lkz::PrimitiveMesh cube{lkz::PrimitiveShape::Box, glm::vec3{4.0f}};
    cube.runtimeTexture = canvas;
    config.meshes = {{cubeMesh, cube}};
    std::unique_ptr<lkz::RenderBackend> backend;
#if defined(LKZ_WITH_NVRHI)
    if (const auto api = lkz::parseGraphicsApi(backendName)) {
        config.title += std::string{" ("} + lkz::graphicsApiName(*api) + ")";
        backend = std::make_unique<lkz::NvrhiRenderBackend>(std::move(config), *api);
    }
#endif
    if (!backend) {
        backend = std::make_unique<lkz::RaylibRenderBackend>(std::move(config));
    }
    lkz::World world{std::move(backend)};
    auto& registry = world.registry();
    auto& box = world.root().emplaceChild<lkz::SceneNode>("Cubo", registry);
    registry.emplace<lkz::RenderComponent>(box.handle(), cubeMesh, 0u, glm::vec4{1.0f});
    box.setRotation(glm::angleAxis(0.6f, glm::vec3{0.0f, 1.0f, 0.0f}));
    auto& camera = world.root().emplaceChild<lkz::Camera>("Câmera", registry);
    const glm::vec3 eye{0.0f, 4.0f, 8.0f};
    camera.setPosition(eye);
    camera.setRotation(glm::quatLookAt(glm::normalize(-eye), glm::vec3{0.0f, 1.0f, 0.0f}));
    world.setActiveCamera(&camera);

    // A cópia na CPU é a imagem de verdade: o pincel pinta nela e só o retângulo mudado
    // vai para a placa.
    lkz::TextureImage image = grid();
    world.uploadTexture(canvas, image);
    int step = 0;
    world.onFixedUpdate([&step, &image](lkz::World& w, float /*dt*/) {
        ++step;
        const float angle = static_cast<float>(step) * 0.05f;
        const int cx = size / 2 + static_cast<int>(std::cos(angle) * 80.0f);
        const int cy = size / 2 + static_cast<int>(std::sin(angle) * 80.0f);
        const std::uint8_t color[3] = {static_cast<std::uint8_t>(128 + 127 * std::sin(angle)),
                                       static_cast<std::uint8_t>(128 + 127 * std::sin(angle + 2.1f)),
                                       static_cast<std::uint8_t>(128 + 127 * std::sin(angle + 4.2f))};
        lkz::TexturePatch patch;
        patch.x = cx - brush / 2;
        patch.y = cy - brush / 2;
        patch.width = brush;
        patch.height = brush;
        patch.strokeEnd = step % 60 == 0;
        patch.rgba.resize(static_cast<std::size_t>(brush * brush * 4));
        for (int y = 0; y < brush; ++y) {
            for (int x = 0; x < brush; ++x) {
                const float dx = static_cast<float>(x) + 0.5f - brush / 2.0f;
                const float dy = static_cast<float>(y) + 0.5f - brush / 2.0f;
                const auto pixel = static_cast<std::size_t>(((patch.y + y) * size + patch.x + x) * 4);
                if (dx * dx + dy * dy <= (brush / 2.0f) * (brush / 2.0f)) {
                    std::copy_n(color, 3, image.rgba.begin() + static_cast<std::ptrdiff_t>(pixel));
                }
                std::copy_n(image.rgba.begin() + static_cast<std::ptrdiff_t>(pixel), 4,
                            patch.rgba.begin() + static_cast<std::ptrdiff_t>((y * brush + x) * 4));
            }
        }
        w.updateTexture(canvas, std::move(patch));
    });
    world.run(seconds > 0.0 ? std::optional{std::chrono::duration<double>{seconds}} : std::nullopt);
    return EXIT_SUCCESS;
}

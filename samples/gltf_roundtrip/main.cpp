// gltf_roundtrip: lê um .glb/.gltf com lkz::asset, grava de novo com saveGlb e mostra um
// relatório dos dois (partes, triângulos, materiais, imagens, caixa, memória de vídeo).
// Com --ver, abre uma janela (raylib, o mesmo carregador de modelos dos jogos) com o
// original à esquerda e a cópia à direita, girando.
//
// Uso: gltf_roundtrip <entrada> <saída.glb> [--ver] [--segundos N]

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>

#include <glm/common.hpp>
#include <glm/gtc/quaternion.hpp>
#include <lkz/Asset/Gltf.hpp>
#include <lkz/Core/Log.hpp>
#include <lkz/Scene/Camera.hpp>
#include <lkz/World.hpp>
#include <lkz/backends/RaylibRenderBackend.hpp>

namespace {

void report(const char* title, const std::filesystem::path& path, const lkz::asset::ModelData& model) {
    glm::vec3 low{std::numeric_limits<float>::max()};
    glm::vec3 high{std::numeric_limits<float>::lowest()};
    for (const auto& part : model.parts) {
        for (const auto& p : part.mesh.positions) {
            low = glm::min(low, p);
            high = glm::max(high, p);
        }
    }
    std::error_code error;
    const auto bytes = std::filesystem::file_size(path, error);
    std::printf("%s: %s\n", title, path.string().c_str());
    std::printf("  %zu partes, %zu triângulos, %zu vértices, %zu materiais, %zu imagens\n", model.parts.size(),
                model.triangleCount(), model.vertexCount(), model.materials.size(), model.images.size());
    std::printf("  caixa: (%.2f, %.2f, %.2f) a (%.2f, %.2f, %.2f)\n", low.x, low.y, low.z, high.x, high.y, high.z);
    std::printf("  arquivo: %.0f KB; memória de vídeo das imagens: %.1f MiB\n", error ? 0.0 : static_cast<double>(bytes) / 1024.0,
                static_cast<double>(model.estimatedTextureMemory()) / (1024.0 * 1024.0));
}

class Spinner final : public lkz::SceneNode {
public:
    using SceneNode::SceneNode;

protected:
    void onUpdate(float dt) override {
        angle_ += dt * 0.4f;
        setRotation(glm::angleAxis(angle_, glm::vec3{0.0f, 1.0f, 0.0f}));
    }

private:
    float angle_ = 0.0f;
};

} // namespace

int main(int argc, char** argv) {
    lkz::initLogging();
    if (argc < 3) {
        std::printf("uso: gltf_roundtrip <entrada> <saída.glb> [--ver] [--segundos N]\n");
        return 1;
    }
    const std::filesystem::path input = argv[1];
    const std::filesystem::path output = argv[2];
    bool view = false;
    double seconds = 0.0;
    for (int i = 3; i < argc; ++i) {
        if (std::strcmp(argv[i], "--ver") == 0) {
            view = true;
        } else if (std::strcmp(argv[i], "--segundos") == 0 && i + 1 < argc) {
            seconds = std::atof(argv[++i]);
        }
    }

    const auto original = lkz::asset::loadGltf(input);
    if (!original) {
        return 2;
    }
    report("original", input, *original);
    if (!lkz::asset::saveGlb(output, *original)) {
        return 3;
    }
    const auto copy = lkz::asset::loadGltf(output);
    if (!copy) {
        return 4;
    }
    report("regravado", output, *copy);
    const bool same = copy->parts.size() == original->parts.size() && copy->triangleCount() == original->triangleCount() &&
                      copy->materials.size() == original->materials.size() && copy->images.size() == original->images.size();
    std::printf("%s\n", same ? "igual: mesmas partes, triângulos, materiais e imagens" : "DIFERENTE");
    if (!view) {
        return same ? 0 : 5;
    }

    // Lado a lado: o mesmo id de mesh por arquivo, o tamanho dele em metros.
    float size = 1.0f;
    for (const auto& part : original->parts) {
        for (const auto& p : part.mesh.positions) {
            size = std::max({size, std::abs(p.x), std::abs(p.y), std::abs(p.z)});
        }
    }
    lkz::RaylibConfig config;
    config.title = "Lkz Engine - gltf_roundtrip (original | regravado)";
    lkz::PrimitiveMesh left{lkz::PrimitiveShape::Model};
    left.model = input.string();
    lkz::PrimitiveMesh right{lkz::PrimitiveShape::Model};
    right.model = output.string();
    config.meshes = {{1, left}, {2, right}, {3, {lkz::PrimitiveShape::Plane}}};
    lkz::World world{std::make_unique<lkz::RaylibRenderBackend>(std::move(config))};
    auto& registry = world.registry();
    auto& ground = world.root().emplaceChild<lkz::SceneNode>("Chão", registry);
    ground.setScale({size * 6.0f, 1.0f, size * 4.0f});
    registry.emplace<lkz::RenderComponent>(ground.handle(), 3u, 0u, glm::vec4{0.3f, 0.32f, 0.35f, 1.0f});
    for (int side = 0; side < 2; ++side) {
        auto& node = world.root().emplaceChild<Spinner>(side == 0 ? "Original" : "Regravado", registry);
        node.setPosition({(side == 0 ? -1.0f : 1.0f) * size * 1.2f, 0.0f, 0.0f});
        registry.emplace<lkz::RenderComponent>(node.handle(), side == 0 ? 1u : 2u, 0u, glm::vec4{1.0f});
    }
    auto& camera = world.root().emplaceChild<lkz::Camera>("Câmera", registry);
    const glm::vec3 eye{0.0f, size * 1.6f, size * 4.2f};
    camera.setPosition(eye);
    camera.setRotation(glm::quatLookAt(glm::normalize(glm::vec3{0.0f, size * 0.6f, 0.0f} - eye), glm::vec3{0.0f, 1.0f, 0.0f}));
    world.setActiveCamera(&camera);
    world.run(seconds > 0.0 ? std::optional{std::chrono::duration<double>{seconds}} : std::nullopt);
    return 0;
}

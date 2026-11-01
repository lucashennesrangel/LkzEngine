#pragma once

#include <cstdint>
#include <vector>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <lkz/Render/MeshData.hpp>
#include <lkz/Render/RenderConfig.hpp>

namespace lkz::nvrhi_detail {

/// Vértice das malhas na GPU (32 bytes).
struct Vertex {
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    glm::vec2 uv{0.0f};
};
static_assert(sizeof(Vertex) == 32);

struct Geometry {
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices; // triângulos, frente anti-horária vista de fora
};

/// A primitiva (Box, Sphere, Cylinder, Plane) ocupando a caixa de `size` centrada em
/// `offset`, como no backend raylib. No Plane, o topo da imagem fica em -Z e a esquerda em -X.
[[nodiscard]] Geometry makePrimitive(PrimitiveShape shape, glm::vec3 size, glm::vec3 offset);

/// A malha do jogo (normais e uvs que faltarem: (0, 1, 0) e (0, 0); sem índices: cada
/// três posições formam um triângulo).
[[nodiscard]] Geometry fromMeshData(const MeshData& mesh);

} // namespace lkz::nvrhi_detail

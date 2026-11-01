#include "Primitives.hpp"

#include <cmath>

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>

namespace lkz::nvrhi_detail {

namespace {

constexpr int sphereRings = 16;
constexpr int sphereSlices = 32;
constexpr int cylinderSlices = 32;

// Quadrado de lado 1 com centro `center`, eixos `u` e `v` (cross(u, v) = normal da frente).
void addQuad(Geometry& geometry, glm::vec3 center, glm::vec3 u, glm::vec3 v) {
    const auto first = static_cast<std::uint32_t>(geometry.vertices.size());
    const glm::vec3 normal = glm::cross(u, v);
    geometry.vertices.push_back({center - u * 0.5f - v * 0.5f, normal, {0.0f, 1.0f}});
    geometry.vertices.push_back({center + u * 0.5f - v * 0.5f, normal, {1.0f, 1.0f}});
    geometry.vertices.push_back({center + u * 0.5f + v * 0.5f, normal, {1.0f, 0.0f}});
    geometry.vertices.push_back({center - u * 0.5f + v * 0.5f, normal, {0.0f, 0.0f}});
    geometry.indices.insert(geometry.indices.end(), {first, first + 1, first + 2, first, first + 2, first + 3});
}

Geometry box() {
    Geometry geometry;
    addQuad(geometry, {0.5f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f});
    addQuad(geometry, {-0.5f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f});
    addQuad(geometry, {0.0f, 0.5f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f});
    addQuad(geometry, {0.0f, -0.5f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f});
    addQuad(geometry, {0.0f, 0.0f, 0.5f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
    addQuad(geometry, {0.0f, 0.0f, -0.5f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
    return geometry;
}

Geometry plane() {
    Geometry geometry;
    // u = x + 0.5 e v = z + 0.5: topo da imagem em -Z, esquerda em -X.
    const glm::vec3 up{0.0f, 1.0f, 0.0f};
    geometry.vertices = {{{-0.5f, 0.0f, -0.5f}, up, {0.0f, 0.0f}},
                         {{-0.5f, 0.0f, 0.5f}, up, {0.0f, 1.0f}},
                         {{0.5f, 0.0f, 0.5f}, up, {1.0f, 1.0f}},
                         {{0.5f, 0.0f, -0.5f}, up, {1.0f, 0.0f}}};
    geometry.indices = {0, 1, 2, 0, 2, 3};
    return geometry;
}

Geometry sphere() {
    Geometry geometry;
    for (int ring = 0; ring <= sphereRings; ++ring) {
        const float theta = glm::pi<float>() * static_cast<float>(ring) / sphereRings;
        for (int slice = 0; slice <= sphereSlices; ++slice) {
            const float phi = glm::two_pi<float>() * static_cast<float>(slice) / sphereSlices;
            const glm::vec3 normal{std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi)};
            geometry.vertices.push_back({normal * 0.5f, normal,
                                         {static_cast<float>(slice) / sphereSlices, static_cast<float>(ring) / sphereRings}});
        }
    }
    const std::uint32_t stride = sphereSlices + 1;
    for (std::uint32_t ring = 0; ring < sphereRings; ++ring) {
        for (std::uint32_t slice = 0; slice < sphereSlices; ++slice) {
            const std::uint32_t a = ring * stride + slice; // em cima
            const std::uint32_t b = a + stride;            // embaixo
            geometry.indices.insert(geometry.indices.end(), {a, b + 1, b, a, a + 1, b + 1});
        }
    }
    return geometry;
}

Geometry cylinder() {
    Geometry geometry;
    for (int slice = 0; slice <= cylinderSlices; ++slice) {
        const float phi = glm::two_pi<float>() * static_cast<float>(slice) / cylinderSlices;
        const glm::vec3 normal{std::cos(phi), 0.0f, std::sin(phi)};
        const float u = static_cast<float>(slice) / cylinderSlices;
        geometry.vertices.push_back({normal * 0.5f + glm::vec3{0.0f, -0.5f, 0.0f}, normal, {u, 1.0f}});
        geometry.vertices.push_back({normal * 0.5f + glm::vec3{0.0f, 0.5f, 0.0f}, normal, {u, 0.0f}});
    }
    for (std::uint32_t slice = 0; slice < cylinderSlices; ++slice) {
        const std::uint32_t bottom = slice * 2;
        const std::uint32_t top = bottom + 1;
        geometry.indices.insert(geometry.indices.end(), {bottom, top + 2, bottom + 2, bottom, top, top + 2});
    }
    // Tampas.
    for (const float side : {1.0f, -1.0f}) {
        const glm::vec3 normal{0.0f, side, 0.0f};
        const auto center = static_cast<std::uint32_t>(geometry.vertices.size());
        geometry.vertices.push_back({normal * 0.5f, normal, {0.5f, 0.5f}});
        for (int slice = 0; slice <= cylinderSlices; ++slice) {
            const float phi = glm::two_pi<float>() * static_cast<float>(slice) / cylinderSlices;
            const glm::vec3 ring{0.5f * std::cos(phi), 0.5f * side, 0.5f * std::sin(phi)};
            geometry.vertices.push_back({ring, normal, {0.5f + ring.x, 0.5f + ring.z}});
        }
        for (std::uint32_t slice = 0; slice < cylinderSlices; ++slice) {
            const std::uint32_t a = center + 1 + slice;
            if (side > 0.0f) {
                geometry.indices.insert(geometry.indices.end(), {center, a + 1, a});
            } else {
                geometry.indices.insert(geometry.indices.end(), {center, a, a + 1});
            }
        }
    }
    return geometry;
}

} // namespace

Geometry makePrimitive(PrimitiveShape shape, glm::vec3 size, glm::vec3 offset) {
    Geometry geometry;
    switch (shape) {
    case PrimitiveShape::Sphere: geometry = sphere(); break;
    case PrimitiveShape::Cylinder: geometry = cylinder(); break;
    case PrimitiveShape::Plane: geometry = plane(); break;
    default: geometry = box(); break;
    }
    for (Vertex& vertex : geometry.vertices) {
        vertex.position = vertex.position * size + offset;
        // Escala diferente por eixo: a normal se transforma pela inversa.
        const glm::vec3 normal = vertex.normal / glm::max(size, glm::vec3{1e-6f});
        vertex.normal = glm::normalize(normal);
    }
    return geometry;
}

Geometry fromMeshData(const MeshData& mesh) {
    Geometry geometry;
    geometry.vertices.resize(mesh.positions.size());
    for (std::size_t i = 0; i < mesh.positions.size(); ++i) {
        Vertex& vertex = geometry.vertices[i];
        vertex.position = mesh.positions[i];
        if (i < mesh.normals.size()) {
            vertex.normal = mesh.normals[i];
        }
        if (i < mesh.uvs.size()) {
            vertex.uv = mesh.uvs[i];
        }
    }
    if (mesh.indices.empty()) {
        geometry.indices.resize(mesh.positions.size() - mesh.positions.size() % 3);
        for (std::size_t i = 0; i < geometry.indices.size(); ++i) {
            geometry.indices[i] = static_cast<std::uint32_t>(i);
        }
    } else {
        geometry.indices = mesh.indices;
    }
    return geometry;
}

} // namespace lkz::nvrhi_detail

#include "MeshSimplify.hpp"

#include <algorithm>
#include <cstddef>
#include <numeric>
#include <vector>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <meshoptimizer.h>

namespace lkz::raylib_detail {

namespace {

struct Vertex {
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f};
    glm::vec2 uv{0.0f};
};

constexpr float preserveError = 0.25f; // deformação máxima (fração do tamanho) ao preservar a forma
constexpr float sloppyMargin = 1.3f;   // ficou acima de meta * margem: tenta a sloppy
constexpr std::size_t maxIndexedVertices = 65535; // índices da raylib são de 16 bits

/// Vértices + triângulos -> Mesh da raylib (indexada se couber em 16 bits; senão,
/// lista de triângulos expandida), já na GPU.
Mesh upload(const std::vector<Vertex>& vertices, const std::vector<unsigned int>& indices, bool normals, bool uvs) {
    Mesh mesh{};
    const bool indexed = vertices.size() <= maxIndexedVertices;
    const std::size_t count = indexed ? vertices.size() : indices.size();
    mesh.vertexCount = static_cast<int>(count);
    mesh.triangleCount = static_cast<int>(indices.size() / 3);
    mesh.vertices = static_cast<float*>(RL_MALLOC(count * 3 * sizeof(float)));
    mesh.normals = normals ? static_cast<float*>(RL_MALLOC(count * 3 * sizeof(float))) : nullptr;
    mesh.texcoords = uvs ? static_cast<float*>(RL_MALLOC(count * 2 * sizeof(float))) : nullptr;
    for (std::size_t i = 0; i < count; ++i) {
        const Vertex& v = vertices[indexed ? i : indices[i]];
        std::copy_n(&v.position.x, 3, mesh.vertices + i * 3);
        if (normals) {
            std::copy_n(&v.normal.x, 3, mesh.normals + i * 3);
        }
        if (uvs) {
            std::copy_n(&v.uv.x, 2, mesh.texcoords + i * 2);
        }
    }
    if (indexed) {
        mesh.indices = static_cast<unsigned short*>(RL_MALLOC(indices.size() * sizeof(unsigned short)));
        std::transform(indices.begin(), indices.end(), mesh.indices,
                       [](unsigned int index) { return static_cast<unsigned short>(index); });
    }
    UploadMesh(&mesh, false);
    return mesh;
}

} // namespace

Mesh simplifyMesh(const Mesh& source, float ratio) {
    if (source.vertexCount <= 0 || source.vertices == nullptr) {
        return {};
    }
    const auto vertexCount = static_cast<std::size_t>(source.vertexCount);
    std::vector<Vertex> vertices(vertexCount);
    for (std::size_t i = 0; i < vertexCount; ++i) {
        vertices[i].position = {source.vertices[i * 3], source.vertices[i * 3 + 1], source.vertices[i * 3 + 2]};
        if (source.normals != nullptr) {
            vertices[i].normal = {source.normals[i * 3], source.normals[i * 3 + 1], source.normals[i * 3 + 2]};
        }
        if (source.texcoords != nullptr) {
            vertices[i].uv = {source.texcoords[i * 2], source.texcoords[i * 2 + 1]};
        }
    }
    std::vector<unsigned int> indices;
    if (source.indices != nullptr) {
        indices.assign(source.indices, source.indices + static_cast<std::size_t>(source.triangleCount) * 3);
    } else {
        indices.resize(vertexCount - vertexCount % 3);
        std::iota(indices.begin(), indices.end(), 0u);
    }
    if (indices.empty()) {
        return {};
    }

    // Junta vértices idênticos (malha sem índices traz cada triângulo separado, e o
    // simplificador precisa saber quem é vizinho de quem).
    std::vector<unsigned int> remap(vertexCount);
    const std::size_t unique = meshopt_generateVertexRemap(remap.data(), indices.data(), indices.size(),
                                                           vertices.data(), vertexCount, sizeof(Vertex));
    meshopt_remapIndexBuffer(indices.data(), indices.data(), indices.size(), remap.data());
    std::vector<Vertex> welded(unique);
    meshopt_remapVertexBuffer(welded.data(), vertices.data(), vertexCount, sizeof(Vertex), remap.data());

    const float* positions = &welded[0].position.x;
    const std::size_t target = std::max<std::size_t>(3, static_cast<std::size_t>(
                                                             static_cast<float>(indices.size()) * ratio) / 3 * 3);
    std::vector<unsigned int> lod(indices.size());
    std::size_t count = meshopt_simplify(lod.data(), indices.data(), indices.size(), positions, unique, sizeof(Vertex),
                                         target, preserveError, 0, nullptr);
    if (static_cast<float>(count) > static_cast<float>(target) * sloppyMargin) {
        count = meshopt_simplifySloppy(lod.data(), indices.data(), indices.size(), positions, unique, sizeof(Vertex),
                                       target, 1.0f, nullptr);
    }
    if (count < 3) {
        return {};
    }
    lod.resize(count);

    // Ordem boa para o cache de vértices da GPU e só os vértices que sobraram.
    meshopt_optimizeVertexCache(lod.data(), lod.data(), count, unique);
    std::vector<unsigned int> fetch(unique);
    const std::size_t used = meshopt_optimizeVertexFetchRemap(fetch.data(), lod.data(), count, unique);
    meshopt_remapIndexBuffer(lod.data(), lod.data(), count, fetch.data());
    std::vector<Vertex> compact(used);
    meshopt_remapVertexBuffer(compact.data(), welded.data(), unique, sizeof(Vertex), fetch.data());

    return upload(compact, lod, source.normals != nullptr, source.texcoords != nullptr);
}

} // namespace lkz::raylib_detail

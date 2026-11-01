#include "lkz/Asset/Unwrap.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <numeric>
#include <unordered_map>
#include <utility>

#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>
#include <lkz/Asset/TexelMap.hpp>
#include <spdlog/spdlog.h>
#include <xatlas.h>

namespace lkz::asset {

namespace {

int wrap(int value, int size) {
    const int r = value % size;
    return r < 0 ? r + size : r;
}

int nextPowerOfTwo(int value) {
    int size = 1;
    while (size < value) {
        size *= 2;
    }
    return size;
}

/// Triângulo com área, pronto para formar cartas.
struct Face {
    int part = -1;
    int triangle = -1;
    int group = -1;
    std::array<std::uint32_t, 3> vertex{}; // índices na parte
    std::array<int, 3> weld{};             // posição soldada (vizinhança entre partes e emendas)
    glm::vec3 normal{0.0f};
    float area = 0.0f;
    int chart = -1;
};

struct PositionKey {
    std::int64_t x;
    std::int64_t y;
    std::int64_t z;
    bool operator==(const PositionKey&) const = default;
};
struct PositionHash {
    std::size_t operator()(const PositionKey& k) const {
        return static_cast<std::size_t>(k.x * 73856093LL ^ k.y * 19349663LL ^ k.z * 83492791LL);
    }
};

/// Eixos da carta plana com normal `n`, vista de frente sem espelhar: em paredes, u na
/// horizontal e v para baixo (o topo da imagem fica em cima); em pisos e telhados, u em +X.
std::pair<glm::vec3, glm::vec3> chartAxes(const glm::vec3& n) {
    const glm::vec3 u = std::abs(n.y) < 0.7f ? glm::normalize(glm::cross(glm::vec3{0.0f, 1.0f, 0.0f}, n))
                                             : glm::normalize(glm::vec3{1.0f, 0.0f, 0.0f} - n * n.x);
    return {u, glm::cross(u, n)};
}

} // namespace

UvOverlap measureUvOverlap(const ModelData& model, int material, int resolution) {
    UvOverlap result;
    if (resolution <= 0) {
        return result;
    }
    std::vector<std::uint8_t> count(static_cast<std::size_t>(resolution) * static_cast<std::size_t>(resolution), 0);
    for (const ModelPart& part : model.parts) {
        if (part.material != material || part.mesh.uvs.size() != part.mesh.positions.size()) {
            continue;
        }
        const std::size_t triangles = triangleCount(part.mesh);
        for (std::size_t t = 0; t < triangles; ++t) {
            const auto index = triangleAt(part.mesh, t);
            const glm::vec2 uv[3] = {part.mesh.uvs[index[0]], part.mesh.uvs[index[1]], part.mesh.uvs[index[2]]};
            // Margem negativa: texels exatamente na aresta comum de dois vizinhos não contam
            // como sobreposição.
            forEachTexel(uv, resolution, resolution, -0.01f, std::nullopt, [&](int x, int y, const glm::vec3&) {
                std::uint8_t& c = count[static_cast<std::size_t>(wrap(y, resolution)) * resolution + wrap(x, resolution)];
                c = static_cast<std::uint8_t>(std::min(c + 1, 255));
            });
        }
    }
    for (const std::uint8_t c : count) {
        result.covered += c > 0 ? 1 : 0;
        result.overlapped += c > 1 ? 1 : 0;
    }
    return result;
}

std::optional<UnwrappedModel> unwrapUnique(const ModelData& source, const UnwrapOptions& options) {
    // Grupos de materiais: os que podem dividir a mesma imagem.
    std::map<std::pair<int, bool>, int> groupOf;
    UnwrappedModel out;
    std::vector<int> partGroup(source.parts.size(), -1);
    for (std::size_t p = 0; p < source.parts.size(); ++p) {
        const int m = source.parts[p].material;
        const MaterialData fallback;
        const MaterialData& material = m >= 0 && m < static_cast<int>(source.materials.size())
                                           ? source.materials[static_cast<std::size_t>(m)]
                                           : fallback;
        const std::pair<int, bool> key{static_cast<int>(material.alphaMode), material.doubleSided};
        auto [it, added] = groupOf.try_emplace(key, static_cast<int>(out.model.materials.size()));
        if (added) {
            MaterialData atlas;
            atlas.alphaMode = material.alphaMode;
            atlas.alphaCutoff = material.alphaCutoff;
            atlas.doubleSided = material.doubleSided;
            atlas.metallic = material.metallic;
            atlas.roughness = material.roughness;
            out.model.materials.push_back(atlas);
        }
        partGroup[p] = it->second;
    }
    for (std::size_t g = 0; g < out.model.materials.size(); ++g) {
        out.model.materials[g].name = out.model.materials.size() == 1 ? "atlas" : "atlas" + std::to_string(g + 1);
    }

    // Faces com área, com as posições soldadas (1/10 mm) para achar vizinhas entre partes.
    std::vector<Face> faces;
    std::unordered_map<PositionKey, int, PositionHash> welds;
    const auto weldOf = [&](const glm::vec3& p) {
        const PositionKey key{std::llround(p.x * 1e4), std::llround(p.y * 1e4), std::llround(p.z * 1e4)};
        return welds.try_emplace(key, static_cast<int>(welds.size())).first->second;
    };
    for (std::size_t p = 0; p < source.parts.size(); ++p) {
        const MeshData& mesh = source.parts[p].mesh;
        const std::size_t triangles = triangleCount(mesh);
        for (std::size_t t = 0; t < triangles; ++t) {
            Face face;
            face.part = static_cast<int>(p);
            face.triangle = static_cast<int>(t);
            face.group = partGroup[p];
            face.vertex = triangleAt(mesh, t);
            const glm::vec3& a = mesh.positions[face.vertex[0]];
            const glm::vec3& b = mesh.positions[face.vertex[1]];
            const glm::vec3& c = mesh.positions[face.vertex[2]];
            const glm::vec3 cross = glm::cross(b - a, c - a);
            const float length = glm::length(cross);
            if (length < 1e-10f) {
                continue; // degenerado: não aparece, não precisa de UV
            }
            face.normal = cross / length;
            face.area = length * 0.5f;
            face.weld = {weldOf(a), weldOf(b), weldOf(c)};
            faces.push_back(face);
        }
    }
    if (faces.empty()) {
        spdlog::warn("unwrapUnique: nenhum triângulo com área");
        return std::nullopt;
    }

    // Arestas (pelas posições soldadas) -> faces.
    std::unordered_map<std::uint64_t, std::vector<int>> edges;
    const auto edgeKey = [](int a, int b) {
        return (static_cast<std::uint64_t>(std::min(a, b)) << 32) | static_cast<std::uint32_t>(std::max(a, b));
    };
    for (std::size_t f = 0; f < faces.size(); ++f) {
        for (int k = 0; k < 3; ++k) {
            edges[edgeKey(faces[f].weld[k], faces[f].weld[(k + 1) % 3])].push_back(static_cast<int>(f));
        }
    }

    // Cartas planas: crescem a partir da maior face ainda livre, pegando vizinhas do mesmo
    // grupo no mesmo plano (ângulo e distância comparados com a semente, sem deriva).
    std::vector<int> order(faces.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](int a, int b) { return faces[a].area > faces[b].area; });
    const float cosLimit = std::cos(glm::radians(options.planarAngle));
    std::vector<glm::vec3> chartNormal;
    int charts = 0;
    for (const int seed : order) {
        if (faces[seed].chart >= 0) {
            continue;
        }
        const int chart = charts++;
        const glm::vec3 n = faces[seed].normal;
        const MeshData& seedMesh = source.parts[static_cast<std::size_t>(faces[seed].part)].mesh;
        const float planeD = glm::dot(n, seedMesh.positions[faces[seed].vertex[0]]);
        chartNormal.push_back(n);
        faces[seed].chart = chart;
        std::vector<int> stack{seed};
        while (!stack.empty()) {
            const int f = stack.back();
            stack.pop_back();
            for (int k = 0; k < 3; ++k) {
                for (const int g : edges[edgeKey(faces[f].weld[k], faces[f].weld[(k + 1) % 3])]) {
                    Face& other = faces[g];
                    if (other.chart >= 0 || other.group != faces[seed].group || glm::dot(other.normal, n) < cosLimit) {
                        continue;
                    }
                    const MeshData& mesh = source.parts[static_cast<std::size_t>(other.part)].mesh;
                    bool onPlane = true;
                    for (const std::uint32_t v : other.vertex) {
                        onPlane = onPlane && std::abs(glm::dot(n, mesh.positions[v]) - planeD) < 0.01f;
                    }
                    if (!onPlane) {
                        continue;
                    }
                    other.chart = chart;
                    stack.push_back(g);
                }
            }
        }
    }
    out.charts = static_cast<std::size_t>(charts);

    // Um atlas por grupo. UV de entrada: a projeção de cada carta no seu plano, em metros
    // (texelsPerUnit vira texels por metro).
    out.texelsPerMeter = options.texelsPerMeter;
    out.imageSize.assign(out.model.materials.size(), glm::ivec2{0});
    out.model.parts.resize(source.parts.size());
    out.source.assign(source.parts.size(), {});
    std::vector<std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t>> remap(source.parts.size());
    for (std::size_t p = 0; p < source.parts.size(); ++p) {
        out.model.parts[p].name = source.parts[p].name;
        out.model.parts[p].material = partGroup[p];
    }
    for (int group = 0; group < static_cast<int>(out.model.materials.size()); ++group) {
        std::vector<int> groupFaces;
        for (std::size_t f = 0; f < faces.size(); ++f) {
            if (faces[f].group == group) {
                groupFaces.push_back(static_cast<int>(f));
            }
        }
        if (groupFaces.empty()) {
            out.imageSize[static_cast<std::size_t>(group)] = {options.minSize, options.minSize};
            continue;
        }
        std::vector<glm::vec2> uvs;
        std::vector<std::uint32_t> indices;
        std::vector<std::uint32_t> faceChart;
        std::map<std::pair<int, int>, std::uint32_t> uvVertex; // (carta, posição soldada) -> vértice
        for (const int f : groupFaces) {
            const Face& face = faces[static_cast<std::size_t>(f)];
            const auto [u, v] = chartAxes(chartNormal[static_cast<std::size_t>(face.chart)]);
            const MeshData& mesh = source.parts[static_cast<std::size_t>(face.part)].mesh;
            for (int k = 0; k < 3; ++k) {
                auto [it, added] = uvVertex.try_emplace({face.chart, face.weld[k]}, static_cast<std::uint32_t>(uvs.size()));
                if (added) {
                    const glm::vec3& position = mesh.positions[face.vertex[k]];
                    uvs.emplace_back(glm::dot(position, u), glm::dot(position, v));
                }
                indices.push_back(it->second);
            }
            faceChart.push_back(static_cast<std::uint32_t>(face.chart));
        }

        xatlas::Atlas* atlas = xatlas::Create();
        xatlas::UvMeshDecl decl;
        decl.vertexUvData = uvs.data();
        decl.vertexCount = static_cast<std::uint32_t>(uvs.size());
        decl.vertexStride = sizeof(glm::vec2);
        decl.indexData = indices.data();
        decl.indexCount = static_cast<std::uint32_t>(indices.size());
        decl.indexFormat = xatlas::IndexFormat::UInt32;
        decl.faceMaterialData = faceChart.data(); // cada carta separada, mesmo que se toquem no UV
        if (const auto error = xatlas::AddUvMesh(atlas, decl); error != xatlas::AddMeshError::Success) {
            spdlog::warn("unwrapUnique: xatlas recusou a malha ({})", xatlas::StringForEnum(error));
            xatlas::Destroy(atlas);
            return std::nullopt;
        }
        const auto start = std::chrono::steady_clock::now();
        xatlas::ComputeCharts(atlas); // com malha de UV: as cartas são as ilhas dela
        // Empacota na densidade pedida; se passar do lado máximo, reduz e tenta de novo.
        // Cada tentativa custa caro: a primeira já parte de uma densidade que cabe pela
        // área (a área das cartas ocupa uns 70% do atlas, com as margens).
        double area = 0.0;
        for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
            const glm::vec2 a = uvs[indices[i + 1]] - uvs[indices[i]];
            const glm::vec2 b = uvs[indices[i + 2]] - uvs[indices[i]];
            area += 0.5 * std::abs(static_cast<double>(a.x * b.y - a.y * b.x));
        }
        float density = options.texelsPerMeter;
        if (area > 0.0) {
            const double fits = static_cast<double>(options.maxSize) * std::sqrt(0.7 / area);
            density = std::min(density, static_cast<float>(fits));
        }
        int attempts = 0;
        for (int attempt = 0; attempt < 12; ++attempt) {
            ++attempts;
            xatlas::PackOptions pack;
            pack.padding = static_cast<std::uint32_t>(options.padding);
            pack.texelsPerUnit = density;
            pack.bilinear = true;
            pack.bruteForce = false;
            xatlas::PackCharts(atlas, pack);
            const int longest = static_cast<int>(std::max(atlas->width, atlas->height));
            if (longest <= options.maxSize) {
                break;
            }
            density *= 0.97f * static_cast<float>(options.maxSize) / static_cast<float>(longest);
        }
        if (atlas->meshCount != 1 || atlas->meshes[0].indexCount != indices.size()) {
            spdlog::warn("unwrapUnique: o xatlas não empacotou o grupo {}", group);
            xatlas::Destroy(atlas);
            return std::nullopt;
        }
        out.texelsPerMeter = std::min(out.texelsPerMeter, density);
        const glm::ivec2 size{std::clamp(nextPowerOfTwo(static_cast<int>(atlas->width)), options.minSize, options.maxSize),
                              std::clamp(nextPowerOfTwo(static_cast<int>(atlas->height)), options.minSize, options.maxSize)};
        out.imageSize[static_cast<std::size_t>(group)] = size;

        // Malhas novas: um vértice por (vértice original, vértice do atlas). A ordem das
        // faces na saída do xatlas é a da entrada.
        const xatlas::Mesh& packed = atlas->meshes[0];
        for (std::size_t i = 0; i < groupFaces.size(); ++i) {
            const Face& face = faces[static_cast<std::size_t>(groupFaces[i])];
            const MeshData& from = source.parts[static_cast<std::size_t>(face.part)].mesh;
            MeshData& to = out.model.parts[static_cast<std::size_t>(face.part)].mesh;
            for (int k = 0; k < 3; ++k) {
                const std::uint32_t atlasVertex = packed.indexArray[i * 3 + static_cast<std::size_t>(k)];
                auto [it, added] = remap[static_cast<std::size_t>(face.part)].try_emplace(
                    {face.vertex[k], atlasVertex}, static_cast<std::uint32_t>(to.positions.size()));
                if (added) {
                    to.positions.push_back(from.positions[face.vertex[k]]);
                    if (from.normals.size() == from.positions.size()) {
                        to.normals.push_back(from.normals[face.vertex[k]]);
                    }
                    const xatlas::Vertex& vertex = packed.vertexArray[atlasVertex];
                    to.uvs.emplace_back(vertex.uv[0] / static_cast<float>(size.x), vertex.uv[1] / static_cast<float>(size.y));
                }
                to.indices.push_back(it->second);
            }
            out.source[static_cast<std::size_t>(face.part)].push_back({face.part, face.triangle});
        }
        spdlog::info("unwrapUnique: grupo {}: {} faces, atlas {}x{} -> imagem {}x{}, {:.1f} px/m, {:.0f}% ocupado "
                     "({} {}, {:.0f} ms)",
                     group, groupFaces.size(), atlas->width, atlas->height, size.x, size.y, density,
                     atlas->utilization != nullptr ? atlas->utilization[0] * 100.0f : 0.0f, attempts,
                     attempts == 1 ? "tentativa" : "tentativas",
                     std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        xatlas::Destroy(atlas);
    }
    // Partes sem normais na origem continuam sem (o leitor calcula).
    for (std::size_t p = 0; p < out.model.parts.size(); ++p) {
        MeshData& mesh = out.model.parts[p].mesh;
        if (mesh.normals.size() != mesh.positions.size()) {
            mesh.normals.clear();
        }
    }
    out.model.extras = source.extras;
    return out;
}

} // namespace lkz::asset

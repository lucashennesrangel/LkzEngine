#include "lkz/Terrain/Drape.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include <glm/geometric.hpp>

namespace lkz {

namespace {

std::uint64_t edgeKey(std::uint32_t a, std::uint32_t b) {
    if (a > b) {
        std::swap(a, b);
    }
    return (static_cast<std::uint64_t>(a) << 32U) | b;
}

constexpr float minUpright = 0.7f; // normal.y acima disto: face virada para cima

} // namespace

void subdivideMesh(MeshData& mesh, const std::function<bool(const glm::vec3&, const glm::vec3&)>& split) {
    if (mesh.positions.empty()) {
        return;
    }
    const std::size_t count = mesh.positions.size();
    const bool hasNormals = mesh.normals.size() == count;
    const bool hasUvs = mesh.uvs.size() == count;
    std::vector<std::uint32_t> source = std::move(mesh.indices);
    if (source.empty()) {
        source.resize(count);
        for (std::size_t i = 0; i < count; ++i) {
            source[i] = static_cast<std::uint32_t>(i);
        }
    }

    // Cada aresta é avaliada uma vez; o ponto do meio é criado uma vez.
    std::unordered_map<std::uint64_t, bool> decided;
    decided.reserve(source.size());
    const auto needs = [&](std::uint32_t a, std::uint32_t b) {
        const auto [it, inserted] = decided.try_emplace(edgeKey(a, b), false);
        if (inserted) {
            it->second = split(mesh.positions[a], mesh.positions[b]);
        }
        return it->second;
    };
    std::unordered_map<std::uint64_t, std::uint32_t> midpoints;
    midpoints.reserve(source.size() / 2);
    const auto midpoint = [&](std::uint32_t a, std::uint32_t b) {
        const auto [it, inserted] = midpoints.try_emplace(edgeKey(a, b), 0U);
        if (inserted) {
            it->second = static_cast<std::uint32_t>(mesh.positions.size());
            // (a + b) * 0.5 é o mesmo número nas duas ordens: a malha do outro lado da
            // aresta chega exatamente ao mesmo ponto.
            mesh.positions.push_back((mesh.positions[a] + mesh.positions[b]) * 0.5f);
            if (hasNormals) {
                const glm::vec3 n = mesh.normals[a] + mesh.normals[b];
                const float length = glm::length(n);
                mesh.normals.push_back(length > 1e-6f ? n / length : mesh.normals[a]);
            }
            if (hasUvs) {
                mesh.uvs.push_back((mesh.uvs[a] + mesh.uvs[b]) * 0.5f);
            }
        }
        return it->second;
    };
    const auto length2 = [&](std::uint32_t a, std::uint32_t b) {
        const glm::vec3 d = mesh.positions[b] - mesh.positions[a];
        return glm::dot(d, d);
    };

    std::vector<std::uint32_t> output;
    output.reserve(source.size());
    std::vector<std::array<std::uint32_t, 3>> stack;
    for (std::size_t t = 0; t + 2 < source.size(); t += 3) {
        stack.push_back({source[t], source[t + 1], source[t + 2]});
        while (!stack.empty()) {
            const auto [a, b, c] = stack.back();
            stack.pop_back();
            if (!needs(a, b) && !needs(b, c) && !needs(c, a)) {
                output.insert(output.end(), {a, b, c});
                continue;
            }
            const float ab = length2(a, b);
            const float bc = length2(b, c);
            const float ca = length2(c, a);
            if (ab >= bc && ab >= ca) {
                const std::uint32_t m = midpoint(a, b);
                stack.push_back({a, m, c});
                stack.push_back({m, b, c});
            } else if (bc >= ca) {
                const std::uint32_t m = midpoint(b, c);
                stack.push_back({a, b, m});
                stack.push_back({a, m, c});
            } else {
                const std::uint32_t m = midpoint(c, a);
                stack.push_back({a, b, m});
                stack.push_back({m, b, c});
            }
        }
    }
    mesh.indices = std::move(output);
}

void subdivideMesh(MeshData& mesh, float maxEdge) {
    if (maxEdge <= 0.0f) {
        return;
    }
    const float limit = maxEdge * maxEdge;
    subdivideMesh(mesh, [limit](const glm::vec3& a, const glm::vec3& b) { return glm::dot(b - a, b - a) > limit; });
}

void drapeMesh(MeshData& mesh, const Heightmap& heightmap, float tolerance) {
    // Arestas mais curtas que isto não pedem mais divisão (dentro de uma célula o relevo
    // é quase plano).
    const float minEdge = heightmap.spacing() * 0.25f;
    const float step = heightmap.spacing() * 0.5f;
    subdivideMesh(mesh, [&](const glm::vec3& first, const glm::vec3& second) {
        // As pontas em ordem fixa: a resposta não depende de quem pergunta.
        const bool swap = std::tie(second.x, second.z) < std::tie(first.x, first.z);
        const glm::vec2 a = swap ? glm::vec2{second.x, second.z} : glm::vec2{first.x, first.z};
        const glm::vec2 b = swap ? glm::vec2{first.x, first.z} : glm::vec2{second.x, second.z};
        const float length = glm::distance(a, b);
        if (length <= minEdge) {
            return false;
        }
        const float ha = heightmap.heightAt(a);
        const float hb = heightmap.heightAt(b);
        const int samples = std::max(2, static_cast<int>(std::ceil(length / step)));
        for (int i = 1; i < samples; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(samples);
            if (std::abs(heightmap.heightAt(a + (b - a) * t) - (ha + (hb - ha) * t)) > tolerance) {
                return true;
            }
        }
        return false;
    });

    const std::size_t count = mesh.positions.size();
    if (mesh.normals.size() != count) {
        mesh.normals.assign(count, glm::vec3{0.0f, 1.0f, 0.0f});
    }
    for (std::size_t i = 0; i < count; ++i) {
        glm::vec3& p = mesh.positions[i];
        p.y += heightmap.heightAt({p.x, p.z});
        if (mesh.normals[i].y > minUpright) {
            mesh.normals[i] = heightmap.normalAt({p.x, p.z});
        }
    }
}

} // namespace lkz

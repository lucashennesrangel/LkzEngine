#include "lkz/Asset/Raycast.hpp"

#include <algorithm>
#include <cmath>

#include <glm/common.hpp>
#include <glm/geometric.hpp>

namespace lkz::asset {

MeshRaycaster::MeshRaycaster(const ModelData& model) {
    for (std::size_t p = 0; p < model.parts.size(); ++p) {
        const MeshData& mesh = model.parts[p].mesh;
        const std::size_t count = asset::triangleCount(mesh);
        for (std::size_t t = 0; t < count; ++t) {
            const auto index = triangleAt(mesh, t);
            if (index[0] >= mesh.positions.size() || index[1] >= mesh.positions.size() ||
                index[2] >= mesh.positions.size()) {
                continue;
            }
            const glm::vec3& a = mesh.positions[index[0]];
            const glm::vec3& b = mesh.positions[index[1]];
            const glm::vec3& c = mesh.positions[index[2]];
            Triangle triangle;
            triangle.a = a;
            triangle.edge1 = b - a;
            triangle.edge2 = c - a;
            triangle.min = glm::min(a, glm::min(b, c));
            triangle.max = glm::max(a, glm::max(b, c));
            triangle.ref = {static_cast<int>(p), static_cast<int>(t)};
            if (triangles_.empty()) {
                min_ = triangle.min;
                max_ = triangle.max;
            } else {
                min_ = glm::min(min_, triangle.min);
                max_ = glm::max(max_, triangle.max);
            }
            triangles_.push_back(triangle);
        }
    }
}

std::optional<RayHit> MeshRaycaster::intersect(const glm::vec3& origin, const glm::vec3& direction,
                                               float maxDistance) const {
    const glm::vec3 dir = glm::normalize(direction);
    std::optional<RayHit> best;
    float bestDistance = maxDistance;
    for (const Triangle& triangle : triangles_) {
        const glm::vec3 p = glm::cross(dir, triangle.edge2);
        const float det = glm::dot(triangle.edge1, p);
        if (std::abs(det) < 1e-12f) {
            continue; // raio paralelo ao triângulo (ou triângulo degenerado)
        }
        const float inverse = 1.0f / det;
        const glm::vec3 s = origin - triangle.a;
        const float u = glm::dot(s, p) * inverse;
        if (u < 0.0f || u > 1.0f) {
            continue;
        }
        const glm::vec3 q = glm::cross(s, triangle.edge1);
        const float v = glm::dot(dir, q) * inverse;
        if (v < 0.0f || u + v > 1.0f) {
            continue;
        }
        const float distance = glm::dot(triangle.edge2, q) * inverse;
        if (distance <= 0.0f || distance >= bestDistance) {
            continue;
        }
        bestDistance = distance;
        best = RayHit{triangle.ref, glm::vec3{1.0f - u - v, u, v}, distance, origin + dir * distance};
    }
    return best;
}

void MeshRaycaster::near(const glm::vec3& center, float radius, std::vector<TriangleRef>& out) const {
    out.clear();
    const float radiusSquared = radius * radius;
    for (const Triangle& triangle : triangles_) {
        const glm::vec3 closest = glm::clamp(center, triangle.min, triangle.max);
        const glm::vec3 offset = closest - center;
        if (glm::dot(offset, offset) <= radiusSquared) {
            out.push_back(triangle.ref);
        }
    }
}

} // namespace lkz::asset

#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include <glm/vec3.hpp>
#include <lkz/Asset/ModelData.hpp>

namespace lkz::asset {

/// Um triângulo do modelo: a parte e o número dele dentro da parte.
struct TriangleRef {
    int part = -1;
    int triangle = -1;
};

/// Onde um raio encontrou a malha.
struct RayHit {
    TriangleRef where;
    glm::vec3 barycentric{0.0f}; // pesos dos vértices 0, 1 e 2 do triângulo (triangleAt)
    float distance = 0.0f;       // ao longo da direção do raio (normalizada)
    glm::vec3 position{0.0f};
};

/// Raio contra as malhas de um modelo (Möller & Trumbore, 1997), por força bruta: serve
/// para modelos de até algumas dezenas de milhares de triângulos (pincel, conta-gotas).
/// Guarda uma cópia dos triângulos; se a malha mudar, crie outro.
class MeshRaycaster {
public:
    MeshRaycaster() = default;
    explicit MeshRaycaster(const ModelData& model);

    /// Primeiro triângulo atingido (de qualquer lado) até `maxDistance`.
    [[nodiscard]] std::optional<RayHit> intersect(const glm::vec3& origin, const glm::vec3& direction,
                                                  float maxDistance = 1e30f) const;
    /// Triângulos cuja caixa envolvente toca a esfera (candidatos; o teste fino é seu).
    void near(const glm::vec3& center, float radius, std::vector<TriangleRef>& out) const;

    [[nodiscard]] std::size_t triangleCount() const { return triangles_.size(); }
    /// Caixa envolvente do modelo inteiro (vazia: min > max).
    [[nodiscard]] const glm::vec3& boundsMin() const { return min_; }
    [[nodiscard]] const glm::vec3& boundsMax() const { return max_; }

private:
    struct Triangle {
        glm::vec3 a{0.0f};
        glm::vec3 edge1{0.0f};
        glm::vec3 edge2{0.0f};
        glm::vec3 min{0.0f};
        glm::vec3 max{0.0f};
        TriangleRef ref;
    };
    std::vector<Triangle> triangles_;
    glm::vec3 min_{1.0f};
    glm::vec3 max_{-1.0f};
};

} // namespace lkz::asset

#pragma once

// Tipos internos do renderer_raylib (não são API pública).

#include <algorithm>
#include <array>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <raylib.h>

namespace lkz::raylib_detail {

/// Uma malha com a sua imagem e a cor do material. Primitivas e malhas do jogo têm uma
/// parte; um modelo tem uma por malha do arquivo.
struct GpuPart {
    Mesh mesh{};
    Texture2D texture{};   // id 0 = sem imagem (usa a branca da raylib); dona: textures, modelo ou nível
    glm::vec4 color{1.0f}; // uniform "materialColor"
};

/// A Matrix da raylib nomeia os campos pelo índice coluna-maior (m0..m3 = primeira
/// coluna), mas os declara linha a linha.
inline Matrix toRaylib(const glm::mat4& m) {
    return Matrix{
        m[0][0], m[1][0], m[2][0], m[3][0],
        m[0][1], m[1][1], m[2][1], m[3][1],
        m[0][2], m[1][2], m[2][2], m[3][2],
        m[0][3], m[1][3], m[2][3], m[3][3],
    };
}

/// Esfera envolvente (no espaço do objeto ou do mundo).
struct Bounds {
    glm::vec3 center{0.0f};
    float radius = 0.0f;
};

/// Esfera que contém os vértices de todas as partes: centro da caixa envolvente.
inline Bounds boundsOf(const std::vector<GpuPart>& parts) {
    glm::vec3 low{1e30f};
    glm::vec3 high{-1e30f};
    for (const auto& part : parts) {
        for (int i = 0; i < part.mesh.vertexCount; ++i) {
            const glm::vec3 v{part.mesh.vertices[i * 3], part.mesh.vertices[i * 3 + 1], part.mesh.vertices[i * 3 + 2]};
            low = glm::min(low, v);
            high = glm::max(high, v);
        }
    }
    if (low.x > high.x) {
        return {};
    }
    Bounds bounds{(low + high) * 0.5f, 0.0f};
    for (const auto& part : parts) {
        for (int i = 0; i < part.mesh.vertexCount; ++i) {
            const glm::vec3 v{part.mesh.vertices[i * 3], part.mesh.vertices[i * 3 + 1], part.mesh.vertices[i * 3 + 2]};
            bounds.radius = std::max(bounds.radius, glm::distance(v, bounds.center));
        }
    }
    return bounds;
}

/// A esfera do objeto levada ao mundo pela matriz de modelo (raio pela maior escala).
inline Bounds transformBounds(const Bounds& local, const glm::mat4& model) {
    const float scale = std::max({glm::length(glm::vec3{model[0]}), glm::length(glm::vec3{model[1]}),
                                  glm::length(glm::vec3{model[2]})});
    return {glm::vec3{model * glm::vec4{local.center, 1.0f}}, local.radius * scale};
}

/// Os 6 planos do volume visível (Gribb e Hartmann), normalizados, apontando para dentro.
class Frustum {
public:
    Frustum() = default; // vê tudo
    explicit Frustum(const glm::mat4& viewProjection) {
        const auto row = [&viewProjection](int i) {
            return glm::vec4{viewProjection[0][i], viewProjection[1][i], viewProjection[2][i], viewProjection[3][i]};
        };
        planes_ = {row(3) + row(0), row(3) - row(0), row(3) + row(1),
                   row(3) - row(1), row(3) + row(2), row(3) - row(2)};
        for (auto& plane : planes_) {
            plane /= glm::length(glm::vec3{plane});
        }
    }

    [[nodiscard]] bool sees(const Bounds& sphere) const {
        return std::all_of(planes_.begin(), planes_.end(), [&sphere](const glm::vec4& plane) {
            return glm::dot(glm::vec3{plane}, sphere.center) + plane.w >= -sphere.radius;
        });
    }

private:
    std::array<glm::vec4, 6> planes_{};
};

} // namespace lkz::raylib_detail

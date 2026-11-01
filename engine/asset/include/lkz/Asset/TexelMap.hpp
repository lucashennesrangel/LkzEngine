#pragma once

#include <algorithm>
#include <cmath>
#include <optional>

#include <glm/geometric.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

namespace lkz::asset {

/// Retângulo de texels [x0, x1) x [y0, y1). Pode passar das bordas da imagem: UV fora de
/// 0..1 se repete, e quem pinta aplica a volta (x mod largura).
struct TexelRect {
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;

    [[nodiscard]] bool empty() const { return x0 >= x1 || y0 >= y1; }
};

/// Percorre os texels de uma imagem width x height cobertos pelo triângulo `uv`
/// (coordenadas glTF: (0, 0) é o canto superior esquerdo da imagem): os de centro dentro
/// dele e os que ficam a até `padding` texels das bordas (com 0, só os de dentro; com ~1,
/// a borda das ilhas não fica sem tinta). Só dentro de `clip`, se houver.
///
/// Chama fn(x, y, barycentric) com x e y sem a volta da repetição e as baricêntricas do
/// ponto do triângulo mais perto do centro do texel (pesos dos vértices 0, 1 e 2).
/// Triângulos sem área no UV são pulados.
template <typename Fn>
void forEachTexel(const glm::vec2 (&uv)[3], int width, int height, float padding,
                  const std::optional<TexelRect>& clip, Fn&& fn) {
    const glm::vec2 size{static_cast<float>(width), static_cast<float>(height)};
    const glm::vec2 p[3] = {uv[0] * size, uv[1] * size, uv[2] * size};
    const glm::vec2 e1 = p[1] - p[0];
    const glm::vec2 e2 = p[2] - p[0];
    const float area = e1.x * e2.y - e1.y * e2.x; // o dobro da área, com sinal
    if (std::abs(area) < 1e-8f) {
        return;
    }
    // Distância (em texels) de cada vértice à aresta oposta: transforma o peso
    // baricêntrico em distância, para a margem `padding`.
    float height3[3];
    for (int i = 0; i < 3; ++i) {
        const float edge = glm::length(p[(i + 2) % 3] - p[(i + 1) % 3]);
        height3[i] = edge > 0.0f ? std::abs(area) / edge : 0.0f;
    }
    const glm::vec2 lo = glm::min(p[0], glm::min(p[1], p[2])) - padding;
    const glm::vec2 hi = glm::max(p[0], glm::max(p[1], p[2])) + padding;
    TexelRect rect{static_cast<int>(std::floor(lo.x - 0.5f)), static_cast<int>(std::floor(lo.y - 0.5f)),
                   static_cast<int>(std::ceil(hi.x - 0.5f)) + 1, static_cast<int>(std::ceil(hi.y - 0.5f)) + 1};
    if (clip) {
        rect = {std::max(rect.x0, clip->x0), std::max(rect.y0, clip->y0), std::min(rect.x1, clip->x1),
                std::min(rect.y1, clip->y1)};
    }
    const float inverse = 1.0f / area;
    for (int y = rect.y0; y < rect.y1; ++y) {
        for (int x = rect.x0; x < rect.x1; ++x) {
            const glm::vec2 q{static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f};
            const glm::vec2 d = q - p[0];
            const float b1 = (d.x * e2.y - d.y * e2.x) * inverse;
            const float b2 = (e1.x * d.y - e1.y * d.x) * inverse;
            glm::vec3 b{1.0f - b1 - b2, b1, b2};
            if (b.x * height3[0] < -padding || b.y * height3[1] < -padding || b.z * height3[2] < -padding) {
                continue;
            }
            if (b.x < 0.0f || b.y < 0.0f || b.z < 0.0f) {
                b = glm::max(b, glm::vec3{0.0f}); // na margem: o ponto mais perto, aproximado
                b /= b.x + b.y + b.z;
            }
            fn(x, y, b);
        }
    }
}

/// Mapa afim de um triângulo 3D para o seu UV: uv(p) = uv0 + (dot(du, p - p0), dot(dv, p - p0))
/// para p no plano do triângulo. `du` e `dv` ficam no plano; os comprimentos deles dizem
/// quanto o UV anda por metro (a densidade de texels é isso vezes o lado da imagem).
struct SurfaceToUv {
    glm::vec3 origin{0.0f};
    glm::vec2 originUv{0.0f};
    glm::vec3 du{0.0f};
    glm::vec3 dv{0.0f};

    [[nodiscard]] glm::vec2 at(const glm::vec3& point) const {
        const glm::vec3 d = point - origin;
        return originUv + glm::vec2{glm::dot(du, d), glm::dot(dv, d)};
    }

    /// Vazio se o triângulo não tem área (em 3D).
    [[nodiscard]] static std::optional<SurfaceToUv> from(const glm::vec3 (&position)[3], const glm::vec2 (&uv)[3]) {
        const glm::vec3 e1 = position[1] - position[0];
        const glm::vec3 e2 = position[2] - position[0];
        const float g11 = glm::dot(e1, e1);
        const float g12 = glm::dot(e1, e2);
        const float g22 = glm::dot(e2, e2);
        const float det = g11 * g22 - g12 * g12;
        if (det <= 1e-20f * std::max(g11 * g22, 1e-30f)) {
            return std::nullopt;
        }
        // Gradiente de uma grandeza linear no plano: resolve dot(g, e1) = a1, dot(g, e2) = a2.
        const auto gradient = [&](float a1, float a2) {
            const float c1 = (g22 * a1 - g12 * a2) / det;
            const float c2 = (g11 * a2 - g12 * a1) / det;
            return e1 * c1 + e2 * c2;
        };
        const glm::vec2 duv1 = uv[1] - uv[0];
        const glm::vec2 duv2 = uv[2] - uv[0];
        return SurfaceToUv{position[0], uv[0], gradient(duv1.x, duv2.x), gradient(duv1.y, duv2.y)};
    }
};

} // namespace lkz::asset

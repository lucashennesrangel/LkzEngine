#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include <glm/vec2.hpp>
#include <lkz/Asset/ModelData.hpp>
#include <lkz/Asset/Raycast.hpp>

namespace lkz::asset {

/// Quanto do UV de um material cai em texels usados por mais de um triângulo. A
/// repetição (UV além de 0..1) também conta: pintar ali pinta todas as cópias.
struct UvOverlap {
    std::size_t covered = 0;    // texels cobertos por algum triângulo
    std::size_t overlapped = 0; // cobertos por dois ou mais
    [[nodiscard]] float ratio() const {
        return covered > 0 ? static_cast<float>(overlapped) / static_cast<float>(covered) : 0.0f;
    }
};
/// Mede numa grade resolution x resolution da imagem do material (partes sem UV não contam).
[[nodiscard]] UvOverlap measureUvOverlap(const ModelData& model, int material, int resolution = 256);

struct UnwrapOptions {
    float texelsPerMeter = 32.0f; // densidade desejada
    int maxSize = 2048;           // lado máximo da imagem: se não couber, a densidade cai
    int minSize = 64;
    int padding = 4;              // texels de folga entre as cartas (mipmaps)
    float planarAngle = 2.0f;     // graus: faces vizinhas até este ângulo entram na mesma carta plana
};

/// Modelo com UV novo, sem sobreposição. Faces coplanares e contíguas formam uma carta
/// plana, projetada sem distorção, em metros e alinhada à vertical. As cartas são
/// empacotadas (xatlas) numa imagem por grupo de materiais: os que têm a mesma
/// transparência e face dupla.
///
/// Os triângulos degenerados ficam de fora. As imagens ainda não existem: ver bakeUnique.
struct UnwrappedModel {
    ModelData model;                              // mesmas partes, na mesma ordem; materiais = grupos
    std::vector<std::vector<TriangleRef>> source; // por parte, por triângulo: o triângulo original
    std::vector<glm::ivec2> imageSize;            // por material novo (potências de 2)
    std::size_t charts = 0;
    float texelsPerMeter = 0.0f;                  // a densidade que coube (pode ser menor que a pedida)
};
[[nodiscard]] std::optional<UnwrappedModel> unwrapUnique(const ModelData& source, const UnwrapOptions& options = {});

} // namespace lkz::asset

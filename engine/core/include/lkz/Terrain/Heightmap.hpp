#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

namespace lkz {

/// Grade regular de alturas (metros) no plano XZ: a amostra (coluna c, linha r) fica em
/// origin + (c, r) * spacing. É a fonte única do relevo: o desenho (TerrainLod), a física
/// (heightfield) e a lógica (altura num ponto, raio do mouse) leem a mesma grade.
class Heightmap {
public:
    Heightmap() = default;
    Heightmap(int columns, int rows, float spacing, glm::vec2 origin, float height = 0.0f);

    /// Grade plana (altura 0) que cobre um retângulo de `size` metros centrado na origem.
    static Heightmap covering(glm::vec2 size, float spacing);

    [[nodiscard]] bool empty() const { return samples_.empty(); }
    [[nodiscard]] int columns() const { return columns_; }
    [[nodiscard]] int rows() const { return rows_; }
    [[nodiscard]] float spacing() const { return spacing_; }
    [[nodiscard]] glm::vec2 origin() const { return origin_; }
    [[nodiscard]] glm::vec2 size() const; // metros cobertos (da primeira à última amostra)

    /// Amostra pela posição na grade (fora dela, a da borda mais próxima).
    [[nodiscard]] float at(int column, int row) const;
    void set(int column, int row, float height);
    [[nodiscard]] std::span<const float> samples() const { return samples_; }
    [[nodiscard]] std::span<float> samples() { return samples_; }

    /// Altura num ponto do mundo (bilinear; fora da grade, a da borda).
    [[nodiscard]] float heightAt(glm::vec2 point) const;
    /// Normal da superfície num ponto (diferenças centrais).
    [[nodiscard]] glm::vec3 normalAt(glm::vec2 point) const;
    /// Menor e maior altura.
    [[nodiscard]] std::pair<float, float> range() const;

    /// Primeiro ponto onde o raio toca a superfície (dentro da grade), até `maxDistance`.
    /// Anda em passos de meia amostra e refina por bisseção.
    [[nodiscard]] std::optional<glm::vec3> raycast(glm::vec3 origin, glm::vec3 direction, float maxDistance) const;

    /// Arquivo binário: cabeçalho e alturas em 16 bits entre a menor e a maior (precisão de
    /// (maior - menor) / 65535). Grava num temporário e troca.
    bool save(const std::filesystem::path& path) const;
    static std::optional<Heightmap> load(const std::filesystem::path& path);

private:
    int columns_ = 0;
    int rows_ = 0;
    float spacing_ = 1.0f;
    glm::vec2 origin_{0.0f};
    std::vector<float> samples_;
};

} // namespace lkz

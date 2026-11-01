#pragma once

#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include "lkz/Terrain/Heightmap.hpp"

namespace lkz {

/// Ajustes do CDLOD (Strugar, "Continuous Distance-Dependent Level of Detail for
/// Rendering Heightmaps", JGT 2009).
struct TerrainLodSettings {
    int gridSize = 32;           // quadrados por lado de cada pedaço (a mesma grade em todos os níveis)
    float firstRange = 0.0f;     // alcance do nível 0 em metros (0 = 2,5 x o lado do pedaço do nível 0)
    float morphStartRatio = 0.7f; // onde o morph para o nível seguinte começa, dentro da faixa
};

/// Um pedaço a desenhar: uma grade de resolution x resolution quadrados esticada sobre o
/// quadrado [origin, origin + size] (XZ), no nível `level`. resolution é gridSize, ou
/// gridSize / 2 quando o pedaço é um quarto de nó (o resto do nó está num nível mais
/// fino): assim o espaçamento dos vértices é sempre o do nível. Os vértices se
/// transformam na grade do nível seguinte (o dobro do espaçamento) entre morphStart e
/// morphEnd de distância da câmera.
struct TerrainNode {
    glm::vec2 origin{0.0f};
    float size = 0.0f;
    int resolution = 0;
    int level = 0;
    float morphStart = 0.0f;
    float morphEnd = 0.0f;
};

/// Seleção CDLOD sobre um Heightmap: quadtree com a menor e a maior altura de cada nó,
/// níveis por faixa de distância (cada faixa o dobro da anterior), descarte pela câmera.
/// Não desenha nada: devolve os pedaços, e o backend desenha a mesma grade em cada um,
/// deslocando a altura e fazendo o morph no vertex shader (sem rachaduras entre níveis).
class TerrainLod {
public:
    TerrainLod() = default;
    TerrainLod(const Heightmap& heightmap, TerrainLodSettings settings = {});

    /// Recalcula a menor e a maior altura dos nós que cobrem a região (colunas e linhas da
    /// grade) depois de uma mudança no relevo.
    void refresh(const Heightmap& heightmap, int column, int row, int columns, int rows);

    /// Pedaços visíveis da câmera em `cameraPosition`, para a matriz `viewProjection`.
    void select(glm::vec3 cameraPosition, const glm::mat4& viewProjection, std::vector<TerrainNode>& out) const;

    [[nodiscard]] int levels() const { return static_cast<int>(levels_.size()); }
    [[nodiscard]] int gridSize() const { return settings_.gridSize; }
    [[nodiscard]] float leafSize() const { return leafSize_; }
    [[nodiscard]] float range(int level) const { return ranges_[static_cast<std::size_t>(level)]; }

private:
    struct Level {
        int columns = 0; // nós por lado neste nível
        int rows = 0;
        float size = 0.0f;
        std::vector<glm::vec2> heights; // (menor, maior) por nó
    };

    void selectNode(int level, int x, int z, glm::vec3 camera, const glm::vec4* planes, std::vector<TerrainNode>& out) const;
    [[nodiscard]] glm::vec2 nodeHeights(int level, int x, int z) const;

    TerrainLodSettings settings_;
    glm::vec2 origin_{0.0f};
    glm::vec2 extent_{0.0f};
    float leafSize_ = 0.0f;
    std::vector<Level> levels_; // [0] = pedaços menores
    std::vector<float> ranges_;
};

} // namespace lkz

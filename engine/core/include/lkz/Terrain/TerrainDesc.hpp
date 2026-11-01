#pragma once

#include <string>
#include <vector>

#include <glm/vec4.hpp>

#include "lkz/Terrain/Heightmap.hpp"
#include "lkz/Terrain/TerrainLod.hpp"

namespace lkz {

/// Terreno desenhado pelo backend (World::setTerrain): o relevo, uma imagem esticada sobre
/// ele inteiro (foto aérea, mapa...) e o nível de detalhe. O backend escolhe os pedaços
/// a cada frame com TerrainLod; a lógica consulta a altura no seu próprio Heightmap.
struct TerrainDesc {
    Heightmap heightmap;          // vazio = remove o terreno
    std::string texture{};        // imagem sobre o terreno inteiro: topo em -Z, esquerda em -X
    glm::vec4 color{1.0f};        // multiplica a imagem
    TerrainLodSettings lod{};
};

/// Mudança num retângulo do relevo (World::updateTerrain): `samples` tem columns x rows
/// alturas, linha a linha, a partir da amostra (column, row).
struct TerrainPatch {
    int column = 0;
    int row = 0;
    int columns = 0;
    int rows = 0;
    std::vector<float> samples{};
};

} // namespace lkz

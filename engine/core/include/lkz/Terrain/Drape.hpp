#pragma once

#include <functional>

#include <glm/vec3.hpp>
#include <lkz/Render/MeshData.hpp>
#include <lkz/Terrain/Heightmap.hpp>

namespace lkz {

/// Divide os triângulos enquanto alguma aresta deles pedir (`split(a, b)` = true),
/// sempre pela metade da aresta mais longa do triângulo (bisseção de Rivara: os
/// triângulos não afinam sem limite, então sempre termina). O ponto novo é o meio exato
/// da aresta, igual nas duas ordens. Posição, normal e uv dos pontos novos são a média
/// dos dois da aresta. A saída é indexada e a frente dos triângulos se mantém.
///
/// Se `split` depende só do comprimento (como na versão com `maxEdge`), malhas vizinhas
/// que dividem uma aresta ganham os mesmos pontos sobre ela, sem frestas.
void subdivideMesh(MeshData& mesh, const std::function<bool(const glm::vec3&, const glm::vec3&)>& split);

/// O mesmo, até nenhuma aresta passar de `maxEdge`.
void subdivideMesh(MeshData& mesh, float maxEdge);

/// Assenta sobre o relevo uma malha feita no plano (y = altura acima do chão). Os
/// triângulos se dividem onde o relevo sob alguma aresta se afasta da reta entre as
/// pontas mais que `tolerance` metros (onde o chão é plano ou uma rampa reta, nada muda);
/// cada vértice sobe a altura do relevo sob ele. As faces viradas para cima ficam com a
/// normal do relevo (luz contínua entre peças vizinhas); as em pé mantêm a sua.
/// Entre malhas vizinhas pode sobrar uma fresta de até `tolerance` onde só uma delas
/// dividiu a aresta comum. Para ruas, calçadas, trilhas, rios e marcas no chão.
void drapeMesh(MeshData& mesh, const Heightmap& heightmap, float tolerance = 0.01f);

} // namespace lkz

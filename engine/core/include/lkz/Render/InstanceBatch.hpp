#pragma once

#include <cstdint>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

namespace lkz {

/// Muitas cópias paradas de uma mesh (floresta, postes, pedras, prédios repetidos),
/// enviadas de uma vez por World::setInstanceBatch. Não há nó nem entidade por cópia:
/// cada uma é só a matriz de modelo (e a cor, se quiser). O backend guarda tudo na
/// placa de vídeo, agrupado em células do plano XZ, e por célula descarta o que está
/// fora da câmera e escolhe o nível de detalhe (LOD/impostor da mesh).
///
/// Para mudar o lote (uma cópia a mais, uma a menos), envie o lote inteiro de novo:
/// é barato perto do custo de desenhar.
struct InstanceBatch {
    std::uint32_t mesh = 0;            // id de RenderComponent::mesh
    std::vector<glm::mat4> transforms{}; // uma por cópia; vazio = remove o lote
    std::vector<glm::vec4> colors{};     // vazio = branco; senão, uma por cópia
    float cellSize = 64.0f;            // lado das células (metros); menor = descarte mais fino, mais draw calls
};

} // namespace lkz

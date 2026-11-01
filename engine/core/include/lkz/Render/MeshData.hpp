#pragma once

#include <cstdint>
#include <vector>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

namespace lkz {

/// Geometria montada pelo próprio jogo (malha procedural), no espaço do objeto.
///
/// `indices` lista triângulos (3 índices cada) sobre `positions`; vazio = cada três
/// posições formam um triângulo. `normals` e `uvs` são por posição; vazios = normal
/// (0, 1, 0) e uv (0, 0). A face da frente é a de ordem anti-horária vista de fora.
struct MeshData {
    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> normals;
    std::vector<glm::vec2> uvs;
    std::vector<std::uint32_t> indices;

    void clear() {
        positions.clear();
        normals.clear();
        uvs.clear();
        indices.clear();
    }
};

} // namespace lkz

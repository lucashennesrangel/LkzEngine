#pragma once

#include <optional>
#include <string>
#include <vector>

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "Images.hpp"
#include "Primitives.hpp"

namespace lkz::nvrhi_detail {

/// Uma parte de um modelo: a geometria de uma primitiva do arquivo, já com a
/// transformação do nó, e o material dela (cor base e imagem, se houver).
struct ModelPart {
    Geometry geometry;
    glm::vec4 color{1.0f};
    std::optional<Image> image; // embutida no arquivo
    std::string imageFile;      // externa: carregada uma vez por arquivo (cache do backend)
};

/// Lê um glTF (.glb ou .gltf com os arquivos ao lado; cgltf). `size` multiplica cada
/// eixo e `offset` desloca, como no backend raylib. Vazio se o arquivo não abre (motivo no log).
[[nodiscard]] std::vector<ModelPart> loadModel(const std::string& path, glm::vec3 size, glm::vec3 offset);

} // namespace lkz::nvrhi_detail

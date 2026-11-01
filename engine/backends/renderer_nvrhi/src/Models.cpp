#include "Models.hpp"

#include <glm/geometric.hpp>
#include <lkz/Asset/Gltf.hpp>
#include <spdlog/spdlog.h>

namespace lkz::nvrhi_detail {

std::vector<ModelPart> loadModel(const std::string& path, glm::vec3 size, glm::vec3 offset) {
    // A leitura do glTF é a da engine (lkz::asset); aqui só vira o formato da GPU.
    asset::GltfLoadOptions options;
    options.externalImagePixels = false; // as externas vão para o cache por arquivo
    const auto model = asset::loadGltf(path, options);
    if (!model) {
        return {};
    }
    std::vector<ModelPart> parts;
    std::size_t triangles = 0;
    for (const asset::ModelPart& source : model->parts) {
        ModelPart part;
        part.geometry = fromMeshData(source.mesh);
        for (Vertex& vertex : part.geometry.vertices) {
            vertex.position = vertex.position * size + offset;
            vertex.normal = glm::normalize(vertex.normal / glm::max(size, glm::vec3{1e-6f}));
        }
        if (source.material >= 0) {
            const asset::MaterialData& material = model->materials[static_cast<std::size_t>(source.material)];
            part.color = material.baseColor;
            if (material.image >= 0) {
                const asset::ImageData& image = model->images[static_cast<std::size_t>(material.image)];
                if (!image.file.empty()) {
                    part.imageFile = image.file.generic_string();
                } else {
                    part.image = Image{image.width, image.height, image.rgba};
                }
            }
        }
        triangles += part.geometry.indices.size() / 3;
        parts.push_back(std::move(part));
    }
    spdlog::info("Modelo: '{}' com {} partes, {} triângulos", path, parts.size(), triangles);
    return parts;
}

} // namespace lkz::nvrhi_detail

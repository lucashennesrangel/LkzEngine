#pragma once

#include <cstddef>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <lkz/Terrain/TerrainDesc.hpp>
#include <raylib.h>

namespace lkz::raylib_detail {

/// Desenha o terreno (World::setTerrain) no estilo CDLOD: a mesma grade instanciada em
/// cada pedaço escolhido pelo TerrainLod, com a altura lida do heightmap (textura de 32
/// bits) e o morph entre níveis no vertex shader.
class TerrainRenderer {
public:
    bool initialize(glm::vec3 lightDirection);
    void release();

    /// Troca o terreno; `texture` é a imagem já carregada (dona: o backend).
    void set(const TerrainDesc& desc, Texture2D texture);
    [[nodiscard]] bool active() const { return heightTexture_ != 0; }
    /// Aplica a mudança num retângulo (textura de alturas e alturas dos nós).
    void update(const TerrainPatch& patch);

    /// Desenha e soma draw calls e triângulos.
    void draw(const glm::mat4& viewProjection, glm::vec3 camera, bool lodColors, std::size_t& drawCalls,
              std::size_t& triangles);

private:
    struct NodeInstance {
        glm::vec4 area;  // origem x, origem z, lado, resolução
        glm::vec4 morph; // início do morph, fim do morph, nível, -
    };

    void clear();

    Shader shader_{};
    int nodeAreaLocation_ = -1;
    int nodeMorphLocation_ = -1;
    int viewProjectionLocation_ = -1;
    int cameraLocation_ = -1;
    int heightsLocation_ = -1;
    int textureLocation_ = -1;
    int areaLocation_ = -1;
    int gridLocation_ = -1;
    int colorLocation_ = -1;
    int lodColorsLocation_ = -1;

    Mesh fullGrid_{};
    Mesh halfGrid_{};
    unsigned int heightTexture_ = 0;
    Texture2D texture_{};
    glm::vec4 color_{1.0f};
    glm::vec4 area_{0.0f};  // origem x, origem z, largura, profundidade
    glm::vec3 grid_{0.0f};  // colunas, linhas, espaçamento
    TerrainLod lod_;
    Heightmap heightmap_; // cópia, para recalcular as alturas dos nós depois de uma mudança

    std::vector<TerrainNode> nodes_;
    std::vector<NodeInstance> instances_;
    unsigned int instanceBuffer_ = 0;
    std::size_t instanceCapacity_ = 0;
};

} // namespace lkz::raylib_detail

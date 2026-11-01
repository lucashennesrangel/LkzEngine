#pragma once

#include <cstddef>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <lkz/Terrain/Heightmap.hpp>
#include <lkz/Terrain/TerrainDesc.hpp>
#include <lkz/Terrain/TerrainLod.hpp>
#include <nvrhi/nvrhi.h>

namespace lkz::nvrhi_detail {

/// Terreno CDLOD (World::setTerrain): seleção dos pedaços na CPU (lkz::TerrainLod) e
/// dois draw calls instanciados (pedaços inteiros e quartos) sobre a textura de alturas.
class TerrainPass {
public:
    void initialize(nvrhi::IDevice* device, const nvrhi::FramebufferInfo& framebuffer);
    void release();

    /// Troca o terreno (heightmap vazio = remove). `albedo`: a imagem sobre ele.
    void set(const TerrainDesc& desc, nvrhi::ITexture* albedo, nvrhi::ISampler* sampler);
    /// Muda um retângulo do relevo (textura e alturas dos nós).
    void update(const TerrainPatch& patch);

    [[nodiscard]] bool active() const { return static_cast<bool>(heights_); }

    /// Escolhe os pedaços (pela câmera e por `cullViewProjection`, no formato do OpenGL) e
    /// desenha numa lista já aberta.
    void draw(nvrhi::ICommandList* list, nvrhi::IFramebuffer* framebuffer, const glm::mat4& viewProjection,
              const glm::mat4& cullViewProjection, glm::vec3 camera, glm::vec3 lightDirection, bool lodColors,
              std::size_t& drawCalls, std::size_t& triangles);

private:
    struct Grid {
        nvrhi::BufferHandle vertices;
        nvrhi::BufferHandle indices;
        std::uint32_t indexCount = 0;
    };
    struct NodeInstance {
        glm::vec4 area;  // origem x, origem z, lado, resolução
        glm::vec4 morph; // início do morph, fim do morph, nível
    };

    Grid makeGrid(int resolution);

    nvrhi::IDevice* device_ = nullptr;
    nvrhi::CommandListHandle uploads_;
    nvrhi::ShaderHandle vertexShader_;
    nvrhi::ShaderHandle pixelShader_;
    nvrhi::InputLayoutHandle inputLayout_;
    nvrhi::BindingLayoutHandle bindingLayout_;
    nvrhi::GraphicsPipelineHandle pipeline_;
    nvrhi::BufferHandle constants_;
    nvrhi::BufferHandle instanceBuffer_;
    std::size_t instanceCapacity_ = 0;

    nvrhi::TextureHandle heights_;
    nvrhi::BindingSetHandle bindings_;
    Grid full_;
    Grid half_;
    Heightmap heightmap_;
    TerrainLod lod_;
    glm::vec4 color_{1.0f};
    std::vector<TerrainNode> nodes_;
    std::vector<NodeInstance> instances_;
};

} // namespace lkz::nvrhi_detail

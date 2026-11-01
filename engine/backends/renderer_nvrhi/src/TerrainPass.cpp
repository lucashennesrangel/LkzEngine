#include "TerrainPass.hpp"

#include <algorithm>
#include <array>

#include <glm/geometric.hpp>
#include <spdlog/spdlog.h>

#include "GraphicsDevice.hpp"
#include <tracy/Tracy.hpp>

#include "terrain_ps.h"
#include "terrain_vs.h"

namespace lkz::nvrhi_detail {

namespace {

struct TerrainConstants {
    glm::mat4 viewProjection;
    glm::vec4 cameraPosition;
    glm::vec4 area;
    glm::vec4 grid;
    glm::vec4 color;
    glm::vec4 lightDirection;
};

} // namespace

void TerrainPass::initialize(nvrhi::IDevice* device, const nvrhi::FramebufferInfo& framebuffer) {
    device_ = device;
    uploads_ = device->createCommandList(commandListParameters());
    const bool vulkan = device->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN;
    nvrhi::ShaderDesc vs;
    vs.shaderType = nvrhi::ShaderType::Vertex;
    vs.entryName = "mainVS";
    vertexShader_ = vulkan ? device->createShader(vs, terrain_vs_spirv, sizeof terrain_vs_spirv)
                           : device->createShader(vs, terrain_vs_dxil, sizeof terrain_vs_dxil);
    nvrhi::ShaderDesc ps;
    ps.shaderType = nvrhi::ShaderType::Pixel;
    ps.entryName = "mainPS";
    pixelShader_ = vulkan ? device->createShader(ps, terrain_ps_spirv, sizeof terrain_ps_spirv)
                          : device->createShader(ps, terrain_ps_dxil, sizeof terrain_ps_dxil);

    std::array<nvrhi::VertexAttributeDesc, 3> attributes;
    attributes[0].setName("POSITION").setFormat(nvrhi::Format::RGB32_FLOAT).setBufferIndex(0).setOffset(0)
        .setElementStride(sizeof(glm::vec3));
    attributes[1].setName("NODEAREA").setFormat(nvrhi::Format::RGBA32_FLOAT).setBufferIndex(1)
        .setOffset(offsetof(NodeInstance, area)).setElementStride(sizeof(NodeInstance)).setIsInstanced(true);
    attributes[2].setName("NODEMORPH").setFormat(nvrhi::Format::RGBA32_FLOAT).setBufferIndex(1)
        .setOffset(offsetof(NodeInstance, morph)).setElementStride(sizeof(NodeInstance)).setIsInstanced(true);
    inputLayout_ = device->createInputLayout(attributes.data(), static_cast<std::uint32_t>(attributes.size()), vertexShader_);

    nvrhi::BindingLayoutDesc layout;
    layout.visibility = nvrhi::ShaderType::All;
    layout.bindings = {
        nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
        nvrhi::BindingLayoutItem::Texture_SRV(0),
        nvrhi::BindingLayoutItem::Texture_SRV(1),
        nvrhi::BindingLayoutItem::Sampler(0),
    };
    bindingLayout_ = device->createBindingLayout(layout);

    nvrhi::BufferDesc constants;
    constants.byteSize = sizeof(TerrainConstants);
    constants.isConstantBuffer = true;
    constants.isVolatile = true;
    constants.maxVersions = 16;
    constants.debugName = "Constantes do terreno";
    constants_ = device->createBuffer(constants);

    nvrhi::GraphicsPipelineDesc desc;
    desc.inputLayout = inputLayout_;
    desc.VS = vertexShader_;
    desc.PS = pixelShader_;
    desc.bindingLayouts = {bindingLayout_};
    desc.renderState.rasterState.frontCounterClockwise = true;
    desc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::Back;
    desc.renderState.depthStencilState.depthTestEnable = true;
    desc.renderState.depthStencilState.depthWriteEnable = true;
    desc.renderState.depthStencilState.depthFunc = nvrhi::ComparisonFunc::GreaterOrEqual;
    pipeline_ = device->createGraphicsPipeline(desc, framebuffer);
}

void TerrainPass::release() {
    heights_ = nullptr;
    bindings_ = nullptr;
    full_ = {};
    half_ = {};
    instanceBuffer_ = nullptr;
    instanceCapacity_ = 0;
    pipeline_ = nullptr;
    constants_ = nullptr;
    bindingLayout_ = nullptr;
    inputLayout_ = nullptr;
    vertexShader_ = nullptr;
    pixelShader_ = nullptr;
    uploads_ = nullptr;
    device_ = nullptr;
}

TerrainPass::Grid TerrainPass::makeGrid(int resolution) {
    const int side = resolution + 1;
    std::vector<glm::vec3> vertices;
    vertices.reserve(static_cast<std::size_t>(side) * static_cast<std::size_t>(side));
    for (int z = 0; z < side; ++z) {
        for (int x = 0; x < side; ++x) {
            vertices.push_back({static_cast<float>(x) / static_cast<float>(resolution), 0.0f,
                                static_cast<float>(z) / static_cast<float>(resolution)});
        }
    }
    std::vector<std::uint32_t> indices;
    for (int z = 0; z < resolution; ++z) {
        for (int x = 0; x < resolution; ++x) {
            const auto a = static_cast<std::uint32_t>(z * side + x);
            const std::uint32_t b = a + 1;
            const auto c = a + static_cast<std::uint32_t>(side);
            const std::uint32_t d = c + 1;
            indices.insert(indices.end(), {a, c, b, b, c, d}); // anti-horário visto de cima
        }
    }
    Grid grid;
    grid.indexCount = static_cast<std::uint32_t>(indices.size());
    nvrhi::BufferDesc vertexDesc;
    vertexDesc.byteSize = vertices.size() * sizeof(glm::vec3);
    vertexDesc.isVertexBuffer = true;
    vertexDesc.debugName = "Grade do terreno";
    vertexDesc.initialState = nvrhi::ResourceStates::VertexBuffer;
    vertexDesc.keepInitialState = true;
    grid.vertices = device_->createBuffer(vertexDesc);
    nvrhi::BufferDesc indexDesc;
    indexDesc.byteSize = indices.size() * sizeof(std::uint32_t);
    indexDesc.isIndexBuffer = true;
    indexDesc.debugName = "Grade do terreno";
    indexDesc.initialState = nvrhi::ResourceStates::IndexBuffer;
    indexDesc.keepInitialState = true;
    grid.indices = device_->createBuffer(indexDesc);
    uploads_->open();
    uploads_->writeBuffer(grid.vertices, vertices.data(), vertexDesc.byteSize);
    uploads_->writeBuffer(grid.indices, indices.data(), indexDesc.byteSize);
    uploads_->close();
    device_->executeCommandList(uploads_);
    return grid;
}

void TerrainPass::set(const TerrainDesc& desc, nvrhi::ITexture* albedo, nvrhi::ISampler* sampler) {
    heights_ = nullptr;
    bindings_ = nullptr;
    if (desc.heightmap.empty() || device_ == nullptr) {
        return;
    }
    const Heightmap& map = desc.heightmap;
    nvrhi::TextureDesc heightsDesc;
    heightsDesc.width = static_cast<std::uint32_t>(map.columns());
    heightsDesc.height = static_cast<std::uint32_t>(map.rows());
    heightsDesc.format = nvrhi::Format::R32_FLOAT;
    heightsDesc.debugName = "Alturas do terreno";
    heightsDesc.initialState = nvrhi::ResourceStates::ShaderResource;
    heightsDesc.keepInitialState = true;
    heights_ = device_->createTexture(heightsDesc);
    uploads_->open();
    uploads_->writeTexture(heights_, 0, 0, map.samples().data(), static_cast<std::size_t>(map.columns()) * sizeof(float));
    uploads_->close();
    device_->executeCommandList(uploads_);

    nvrhi::BindingSetDesc bindings;
    bindings.bindings = {
        nvrhi::BindingSetItem::ConstantBuffer(0, constants_),
        nvrhi::BindingSetItem::Texture_SRV(0, albedo),
        nvrhi::BindingSetItem::Texture_SRV(1, heights_),
        nvrhi::BindingSetItem::Sampler(0, sampler),
    };
    bindings_ = device_->createBindingSet(bindings, bindingLayout_);

    lod_ = TerrainLod{map, desc.lod};
    heightmap_ = map;
    full_ = makeGrid(lod_.gridSize());
    half_ = makeGrid(lod_.gridSize() / 2);
    color_ = desc.color;
    spdlog::info("NVRHI: terreno {}x{} amostras de {} m, {} níveis, pedaço de {} m", map.columns(), map.rows(),
                 map.spacing(), lod_.levels(), lod_.leafSize());
}

void TerrainPass::update(const TerrainPatch& patch) {
    if (!active() || patch.columns <= 0 || patch.rows <= 0 ||
        patch.samples.size() != static_cast<std::size_t>(patch.columns) * static_cast<std::size_t>(patch.rows) ||
        patch.column < 0 || patch.row < 0 || patch.column + patch.columns > heightmap_.columns() ||
        patch.row + patch.rows > heightmap_.rows()) {
        spdlog::warn("NVRHI: mudança de relevo fora da grade; ignorada");
        return;
    }
    for (int r = 0; r < patch.rows; ++r) {
        for (int c = 0; c < patch.columns; ++c) {
            heightmap_.set(patch.column + c, patch.row + r,
                           patch.samples[static_cast<std::size_t>(r) * static_cast<std::size_t>(patch.columns) + static_cast<std::size_t>(c)]);
        }
    }
    // O pedaço vai numa textura do tamanho dele e é copiado para o lugar na GPU.
    nvrhi::TextureDesc desc;
    desc.width = static_cast<std::uint32_t>(patch.columns);
    desc.height = static_cast<std::uint32_t>(patch.rows);
    desc.format = nvrhi::Format::R32_FLOAT;
    desc.debugName = "Pedaço do relevo";
    desc.initialState = nvrhi::ResourceStates::CopySource;
    desc.keepInitialState = true;
    const nvrhi::TextureHandle piece = device_->createTexture(desc);
    uploads_->open();
    uploads_->writeTexture(piece, 0, 0, patch.samples.data(), static_cast<std::size_t>(patch.columns) * sizeof(float));
    uploads_->copyTexture(heights_,
                          nvrhi::TextureSlice()
                              .setOrigin(static_cast<std::uint32_t>(patch.column), static_cast<std::uint32_t>(patch.row))
                              .setSize(desc.width, desc.height, 1),
                          piece, nvrhi::TextureSlice());
    uploads_->close();
    device_->executeCommandList(uploads_);
    lod_.refresh(heightmap_, patch.column, patch.row, patch.columns, patch.rows);
}

void TerrainPass::draw(nvrhi::ICommandList* list, nvrhi::IFramebuffer* framebuffer, const glm::mat4& viewProjection,
                       const glm::mat4& cullViewProjection, glm::vec3 camera, glm::vec3 lightDirection, bool lodColors,
                       std::size_t& drawCalls, std::size_t& triangles) {
    if (!active()) {
        return;
    }
    ZoneScopedN("TerrainPass::draw");
    lod_.select(camera, cullViewProjection, nodes_);
    if (nodes_.empty()) {
        return;
    }
    // Pedaços inteiros primeiro, quartos depois: um draw call por grade.
    std::stable_partition(nodes_.begin(), nodes_.end(),
                          [this](const TerrainNode& node) { return node.resolution == lod_.gridSize(); });
    instances_.clear();
    std::size_t fullCount = 0;
    for (const TerrainNode& node : nodes_) {
        instances_.push_back({{node.origin.x, node.origin.y, node.size, static_cast<float>(node.resolution)},
                              {node.morphStart, node.morphEnd, static_cast<float>(node.level), 0.0f}});
        fullCount += node.resolution == lod_.gridSize() ? 1 : 0;
    }
    if (instances_.size() > instanceCapacity_) {
        instanceCapacity_ = std::max(instances_.size(), instanceCapacity_ * 2);
        nvrhi::BufferDesc desc;
        desc.byteSize = instanceCapacity_ * sizeof(NodeInstance);
        desc.isVertexBuffer = true;
        desc.debugName = "Pedaços do terreno";
        desc.initialState = nvrhi::ResourceStates::VertexBuffer;
        desc.keepInitialState = true;
        instanceBuffer_ = device_->createBuffer(desc);
    }
    list->writeBuffer(instanceBuffer_, instances_.data(), instances_.size() * sizeof(NodeInstance));

    const TerrainConstants constants{
        viewProjection,
        glm::vec4{camera, 0.0f},
        glm::vec4{heightmap_.origin().x, heightmap_.origin().y, heightmap_.size().x, heightmap_.size().y},
        glm::vec4{static_cast<float>(heightmap_.columns()), static_cast<float>(heightmap_.rows()), heightmap_.spacing(),
                  lodColors ? 1.0f : 0.0f},
        color_,
        glm::vec4{glm::normalize(lightDirection), 0.0f},
    };
    list->writeBuffer(constants_, &constants, sizeof constants);

    const auto drawGrid = [&](const Grid& grid, std::size_t first, std::size_t count) {
        if (count == 0) {
            return;
        }
        nvrhi::GraphicsState state;
        state.pipeline = pipeline_;
        state.framebuffer = framebuffer;
        state.viewport = nvrhi::ViewportState().addViewportAndScissorRect(framebuffer->getFramebufferInfo().getViewport());
        state.bindings = {bindings_};
        state.vertexBuffers = {
            nvrhi::VertexBufferBinding().setBuffer(grid.vertices).setSlot(0).setOffset(0),
            nvrhi::VertexBufferBinding().setBuffer(instanceBuffer_).setSlot(1).setOffset(first * sizeof(NodeInstance)),
        };
        state.indexBuffer = nvrhi::IndexBufferBinding().setBuffer(grid.indices).setFormat(nvrhi::Format::R32_UINT);
        list->setGraphicsState(state);
        list->drawIndexed(nvrhi::DrawArguments().setVertexCount(grid.indexCount).setInstanceCount(static_cast<std::uint32_t>(count)));
        ++drawCalls;
        triangles += static_cast<std::size_t>(grid.indexCount / 3) * count;
    };
    drawGrid(full_, 0, fullCount);
    drawGrid(half_, fullCount, instances_.size() - fullCount);
}

} // namespace lkz::nvrhi_detail

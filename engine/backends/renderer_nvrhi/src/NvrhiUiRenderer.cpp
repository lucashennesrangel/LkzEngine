#include "NvrhiUiRenderer.hpp"

#include <algorithm>
#include <array>
#include <cstddef>

#include <RmlUi/Core/Vertex.h>
#include <glm/gtc/type_ptr.hpp>
#include <spdlog/spdlog.h>

#include "GraphicsDevice.hpp"

#include "Images.hpp"
#include "ui_ps.h"
#include "ui_vs.h"

namespace lkz::nvrhi_detail {

namespace {

struct DrawConstants {
    glm::mat4 transform;
    glm::vec4 translation;
};
static_assert(sizeof(Rml::Vertex) == 20);

} // namespace

NvrhiUiRenderer::NvrhiUiRenderer(nvrhi::IDevice* device, const nvrhi::FramebufferInfo& framebuffer, nvrhi::ITexture* white)
    : device_{device}
    , framebufferInfo_{framebuffer} {
    white_ = new Texture{white, nullptr};
}

NvrhiUiRenderer::~NvrhiUiRenderer() {
    delete white_;
}

void NvrhiUiRenderer::setTarget(nvrhi::ICommandList* list, nvrhi::IFramebuffer* framebuffer) {
    list_ = list;
    framebuffer_ = framebuffer;
}

bool NvrhiUiRenderer::initialize() {
    uploads_ = device_->createCommandList(commandListParameters());
    const bool vulkan = device_->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN;
    nvrhi::ShaderDesc vs;
    vs.shaderType = nvrhi::ShaderType::Vertex;
    vs.entryName = "mainVS";
    vertexShader_ = vulkan ? device_->createShader(vs, ui_vs_spirv, sizeof ui_vs_spirv)
                           : device_->createShader(vs, ui_vs_dxil, sizeof ui_vs_dxil);
    nvrhi::ShaderDesc ps;
    ps.shaderType = nvrhi::ShaderType::Pixel;
    ps.entryName = "mainPS";
    pixelShader_ = vulkan ? device_->createShader(ps, ui_ps_spirv, sizeof ui_ps_spirv)
                          : device_->createShader(ps, ui_ps_dxil, sizeof ui_ps_dxil);

    std::array<nvrhi::VertexAttributeDesc, 3> attributes;
    attributes[0].setName("POSITION").setFormat(nvrhi::Format::RG32_FLOAT).setOffset(offsetof(Rml::Vertex, position))
        .setElementStride(sizeof(Rml::Vertex));
    attributes[1].setName("COLOR").setFormat(nvrhi::Format::RGBA8_UNORM).setOffset(offsetof(Rml::Vertex, colour))
        .setElementStride(sizeof(Rml::Vertex));
    attributes[2].setName("TEXCOORD").setFormat(nvrhi::Format::RG32_FLOAT).setOffset(offsetof(Rml::Vertex, tex_coord))
        .setElementStride(sizeof(Rml::Vertex));
    inputLayout_ = device_->createInputLayout(attributes.data(), static_cast<std::uint32_t>(attributes.size()), vertexShader_);

    nvrhi::BindingLayoutDesc layout;
    layout.visibility = nvrhi::ShaderType::All;
    layout.bindings = {
        nvrhi::BindingLayoutItem::PushConstants(0, sizeof(DrawConstants)),
        nvrhi::BindingLayoutItem::Texture_SRV(0),
        nvrhi::BindingLayoutItem::Sampler(0),
    };
    bindingLayout_ = device_->createBindingLayout(layout);

    nvrhi::SamplerDesc samplerDesc;
    samplerDesc.setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
    samplerDesc.setAllFilters(true);
    sampler_ = device_->createSampler(samplerDesc);

    nvrhi::GraphicsPipelineDesc desc;
    desc.inputLayout = inputLayout_;
    desc.VS = vertexShader_;
    desc.PS = pixelShader_;
    desc.bindingLayouts = {bindingLayout_};
    desc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
    desc.renderState.rasterState.scissorEnable = true;
    desc.renderState.depthStencilState.depthTestEnable = false;
    desc.renderState.depthStencilState.depthWriteEnable = false;
    nvrhi::BlendState::RenderTarget& blend = desc.renderState.blendState.targets[0];
    blend.blendEnable = true; // cores pré-multiplicadas
    blend.srcBlend = nvrhi::BlendFactor::One;
    blend.destBlend = nvrhi::BlendFactor::InvSrcAlpha;
    blend.srcBlendAlpha = nvrhi::BlendFactor::One;
    blend.destBlendAlpha = nvrhi::BlendFactor::InvSrcAlpha;
    pipeline_ = device_->createGraphicsPipeline(desc, framebufferInfo_);

    nvrhi::BindingSetDesc bindings;
    bindings.bindings = {
        nvrhi::BindingSetItem::PushConstants(0, sizeof(DrawConstants)),
        nvrhi::BindingSetItem::Texture_SRV(0, white_->texture),
        nvrhi::BindingSetItem::Sampler(0, sampler_),
    };
    white_->bindings = device_->createBindingSet(bindings, bindingLayout_);
    return static_cast<bool>(pipeline_);
}

void NvrhiUiRenderer::release() {
    delete white_;
    white_ = nullptr;
    pipeline_ = nullptr;
    sampler_ = nullptr;
    bindingLayout_ = nullptr;
    inputLayout_ = nullptr;
    vertexShader_ = nullptr;
    pixelShader_ = nullptr;
    uploads_ = nullptr;
}

void NvrhiUiRenderer::beginFrame(int width, int height) {
    width_ = width;
    height_ = height;
    // Pixels (origem no canto superior esquerdo, y para baixo) para a tela.
    projection_ = glm::mat4{1.0f};
    projection_[0][0] = 2.0f / static_cast<float>(width);
    projection_[1][1] = -2.0f / static_cast<float>(height);
    projection_[3][0] = -1.0f;
    projection_[3][1] = 1.0f;
    transform_ = glm::mat4{1.0f};
    scissor_ = false;
}

void NvrhiUiRenderer::endFrame() {
    list_ = nullptr;
    framebuffer_ = nullptr;
}

Rml::CompiledGeometryHandle NvrhiUiRenderer::CompileGeometry(Rml::Span<const Rml::Vertex> vertices,
                                                             Rml::Span<const int> indices) {
    auto* geometry = new Geometry;
    geometry->count = static_cast<std::uint32_t>(indices.size());
    if (vertices.empty() || indices.empty()) {
        return reinterpret_cast<Rml::CompiledGeometryHandle>(geometry);
    }
    nvrhi::BufferDesc vertexDesc;
    vertexDesc.byteSize = vertices.size() * sizeof(Rml::Vertex);
    vertexDesc.isVertexBuffer = true;
    vertexDesc.debugName = "Interface";
    vertexDesc.initialState = nvrhi::ResourceStates::VertexBuffer;
    vertexDesc.keepInitialState = true;
    geometry->vertices = device_->createBuffer(vertexDesc);
    nvrhi::BufferDesc indexDesc;
    indexDesc.byteSize = indices.size() * sizeof(int);
    indexDesc.isIndexBuffer = true;
    indexDesc.debugName = "Interface";
    indexDesc.initialState = nvrhi::ResourceStates::IndexBuffer;
    indexDesc.keepInitialState = true;
    geometry->indices = device_->createBuffer(indexDesc);
    uploads_->open();
    uploads_->writeBuffer(geometry->vertices, vertices.data(), vertexDesc.byteSize);
    uploads_->writeBuffer(geometry->indices, indices.data(), indexDesc.byteSize);
    uploads_->close();
    device_->executeCommandList(uploads_);
    return reinterpret_cast<Rml::CompiledGeometryHandle>(geometry);
}

void NvrhiUiRenderer::RenderGeometry(Rml::CompiledGeometryHandle handle, Rml::Vector2f translation,
                                     Rml::TextureHandle texture) {
    const auto* geometry = reinterpret_cast<const Geometry*>(handle);
    if (list_ == nullptr || geometry->count == 0) {
        return;
    }
    const auto* image = texture != 0 ? reinterpret_cast<const Texture*>(texture) : white_;
    nvrhi::GraphicsState state;
    state.pipeline = pipeline_;
    state.framebuffer = framebuffer_;
    const nvrhi::Viewport viewport{static_cast<float>(width_), static_cast<float>(height_)};
    state.viewport.addViewport(viewport);
    nvrhi::Rect scissor = nvrhi::Rect(viewport);
    if (scissor_) {
        // Dentro da janela: o Vulkan não aceita recorte com origem negativa.
        scissor.minX = std::clamp(scissorRect_.minX, 0, width_);
        scissor.maxX = std::clamp(scissorRect_.maxX, scissor.minX, width_);
        scissor.minY = std::clamp(scissorRect_.minY, 0, height_);
        scissor.maxY = std::clamp(scissorRect_.maxY, scissor.minY, height_);
    }
    state.viewport.addScissorRect(scissor);
    state.bindings = {image->bindings};
    state.vertexBuffers = {nvrhi::VertexBufferBinding().setBuffer(geometry->vertices).setSlot(0).setOffset(0)};
    state.indexBuffer = nvrhi::IndexBufferBinding().setBuffer(geometry->indices).setFormat(nvrhi::Format::R32_UINT);
    list_->setGraphicsState(state);
    const DrawConstants constants{projection_ * transform_, glm::vec4{translation.x, translation.y, 0.0f, 0.0f}};
    list_->setPushConstants(&constants, sizeof constants);
    list_->drawIndexed(nvrhi::DrawArguments().setVertexCount(geometry->count));
}

void NvrhiUiRenderer::ReleaseGeometry(Rml::CompiledGeometryHandle handle) {
    delete reinterpret_cast<Geometry*>(handle); // o NVRHI só solta os buffers quando a GPU termina
}

Rml::TextureHandle NvrhiUiRenderer::addTexture(nvrhi::TextureHandle texture) {
    auto* entry = new Texture{std::move(texture), nullptr};
    nvrhi::BindingSetDesc bindings;
    bindings.bindings = {
        nvrhi::BindingSetItem::PushConstants(0, sizeof(DrawConstants)),
        nvrhi::BindingSetItem::Texture_SRV(0, entry->texture),
        nvrhi::BindingSetItem::Sampler(0, sampler_),
    };
    entry->bindings = device_->createBindingSet(bindings, bindingLayout_);
    return reinterpret_cast<Rml::TextureHandle>(entry);
}

Rml::TextureHandle NvrhiUiRenderer::LoadTexture(Rml::Vector2i& dimensions, const Rml::String& source) {
    auto image = loadImage(source);
    if (!image) {
        spdlog::warn("Interface: não foi possível carregar a imagem '{}'", source);
        return 0;
    }
    // A RmlUi trabalha com alfa pré-multiplicado.
    for (std::size_t i = 0; i + 3 < image->pixels.size(); i += 4) {
        const unsigned alpha = image->pixels[i + 3];
        for (std::size_t c = 0; c < 3; ++c) {
            image->pixels[i + c] = static_cast<std::uint8_t>(image->pixels[i + c] * alpha / 255u);
        }
    }
    dimensions = {image->width, image->height};
    const std::vector<Image> chain = mipChain(std::move(*image));
    nvrhi::TextureDesc desc;
    desc.width = static_cast<std::uint32_t>(dimensions.x);
    desc.height = static_cast<std::uint32_t>(dimensions.y);
    desc.mipLevels = static_cast<std::uint32_t>(chain.size());
    desc.format = nvrhi::Format::RGBA8_UNORM;
    desc.debugName = source.c_str();
    desc.initialState = nvrhi::ResourceStates::ShaderResource;
    desc.keepInitialState = true;
    nvrhi::TextureHandle texture = device_->createTexture(desc);
    uploads_->open();
    for (std::size_t level = 0; level < chain.size(); ++level) {
        uploads_->writeTexture(texture, 0, static_cast<std::uint32_t>(level), chain[level].pixels.data(),
                               static_cast<std::size_t>(chain[level].width) * 4);
    }
    uploads_->close();
    device_->executeCommandList(uploads_);
    return addTexture(std::move(texture));
}

Rml::TextureHandle NvrhiUiRenderer::GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i dimensions) {
    nvrhi::TextureDesc desc;
    desc.width = static_cast<std::uint32_t>(dimensions.x);
    desc.height = static_cast<std::uint32_t>(dimensions.y);
    desc.format = nvrhi::Format::RGBA8_UNORM;
    desc.debugName = "Interface (gerada)";
    desc.initialState = nvrhi::ResourceStates::ShaderResource;
    desc.keepInitialState = true;
    nvrhi::TextureHandle texture = device_->createTexture(desc);
    uploads_->open();
    uploads_->writeTexture(texture, 0, 0, source.data(), static_cast<std::size_t>(dimensions.x) * 4);
    uploads_->close();
    device_->executeCommandList(uploads_);
    return addTexture(std::move(texture));
}

void NvrhiUiRenderer::ReleaseTexture(Rml::TextureHandle texture) {
    delete reinterpret_cast<Texture*>(texture);
}

void NvrhiUiRenderer::EnableScissorRegion(bool enable) {
    scissor_ = enable;
}

void NvrhiUiRenderer::SetScissorRegion(Rml::Rectanglei region) {
    scissorRect_ = nvrhi::Rect(region.Left(), region.Left() + region.Width(), region.Top(), region.Top() + region.Height());
}

void NvrhiUiRenderer::SetTransform(const Rml::Matrix4f* transform) {
    // Matrix4f da RmlUi guarda por colunas, como o glm.
    transform_ = transform != nullptr ? glm::make_mat4(transform->data()) : glm::mat4{1.0f};
}

} // namespace lkz::nvrhi_detail

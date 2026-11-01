#pragma once

#include <glm/mat4x4.hpp>
#include <lkz/ui_rmlui/UiLayer.hpp>
#include <nvrhi/nvrhi.h>

namespace lkz::nvrhi_detail {

/// O desenho da interface (RmlUi) no NVRHI: geometria em buffers, texturas com alfa
/// pré-multiplicado, recorte por scissor e transformações por matriz. Desenha na lista
/// de comandos do frame (setTarget antes de UiLayer::render).
class NvrhiUiRenderer final : public ui_rmlui::UiRenderer {
public:
    NvrhiUiRenderer(nvrhi::IDevice* device, const nvrhi::FramebufferInfo& framebuffer, nvrhi::ITexture* white);
    ~NvrhiUiRenderer() override;
    NvrhiUiRenderer(const NvrhiUiRenderer&) = delete;
    NvrhiUiRenderer& operator=(const NvrhiUiRenderer&) = delete;

    /// Onde desenhar o próximo UiLayer::render.
    void setTarget(nvrhi::ICommandList* list, nvrhi::IFramebuffer* framebuffer);

    bool initialize() override;
    void release() override;
    void beginFrame(int width, int height) override;
    void endFrame() override;

    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) override;
    void RenderGeometry(Rml::CompiledGeometryHandle handle, Rml::Vector2f translation, Rml::TextureHandle texture) override;
    void ReleaseGeometry(Rml::CompiledGeometryHandle handle) override;
    Rml::TextureHandle LoadTexture(Rml::Vector2i& dimensions, const Rml::String& source) override;
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i dimensions) override;
    void ReleaseTexture(Rml::TextureHandle texture) override;
    void EnableScissorRegion(bool enable) override;
    void SetScissorRegion(Rml::Rectanglei region) override;
    void SetTransform(const Rml::Matrix4f* transform) override;

private:
    struct Geometry {
        nvrhi::BufferHandle vertices;
        nvrhi::BufferHandle indices;
        std::uint32_t count = 0;
    };
    struct Texture {
        nvrhi::TextureHandle texture;
        nvrhi::BindingSetHandle bindings;
    };

    Rml::TextureHandle addTexture(nvrhi::TextureHandle texture);

    nvrhi::IDevice* device_;
    nvrhi::FramebufferInfo framebufferInfo_;
    nvrhi::CommandListHandle uploads_;
    nvrhi::ShaderHandle vertexShader_;
    nvrhi::ShaderHandle pixelShader_;
    nvrhi::InputLayoutHandle inputLayout_;
    nvrhi::BindingLayoutHandle bindingLayout_;
    nvrhi::GraphicsPipelineHandle pipeline_;
    nvrhi::SamplerHandle sampler_;
    Texture* white_ = nullptr; // textura 0 da RmlUi: só a cor

    nvrhi::ICommandList* list_ = nullptr;
    nvrhi::IFramebuffer* framebuffer_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    glm::mat4 projection_{1.0f};
    glm::mat4 transform_{1.0f};
    bool scissor_ = false;
    nvrhi::Rect scissorRect_;
};

} // namespace lkz::nvrhi_detail

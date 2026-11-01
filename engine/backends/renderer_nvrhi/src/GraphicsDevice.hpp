#pragma once

#include <cstdint>
#include <memory>

#include <nvrhi/nvrhi.h>

struct SDL_Window;

namespace lkz::nvrhi_detail {

/// Device do NVRHI e swapchain de uma janela, numa API (D3D12 ou Vulkan). O resto do
/// backend só fala com nvrhi::IDevice.
class GraphicsDevice {
public:
    virtual ~GraphicsDevice() = default;

    /// Cria device e swapchain. false se a API não está disponível (motivo no log).
    virtual bool create(SDL_Window* window, int width, int height, bool vsync, bool debug) = 0;
    virtual void destroy() = 0;

    /// O device para criar recursos e executar comandos (com a validação, se ligada).
    [[nodiscard]] virtual nvrhi::IDevice* device() const = 0;
    /// Nome do adaptador (placa de vídeo).
    [[nodiscard]] virtual const char* adapterName() const = 0;

    /// Escolhe a imagem da swapchain do frame. false: a swapchain ficou velha (chame
    /// resize e pule o frame).
    virtual bool acquire() = 0;
    /// Mostra a imagem do frame (depois de executar os comandos dele).
    virtual void present() = 0;
    /// Refaz a swapchain no novo tamanho (espera a GPU terminar).
    virtual void resize(int width, int height) = 0;

    /// Formato das imagens da swapchain (o mesmo enquanto o device existir).
    [[nodiscard]] virtual nvrhi::Format format() const = 0;
    /// 0 enquanto a janela está minimizada (sem swapchain).
    [[nodiscard]] virtual std::uint32_t backBufferCount() const = 0;
    [[nodiscard]] virtual std::uint32_t backBufferIndex() const = 0;
    [[nodiscard]] virtual nvrhi::ITexture* backBuffer(std::uint32_t index) const = 0;
};

std::unique_ptr<GraphicsDevice> createD3D12Device();
std::unique_ptr<GraphicsDevice> createVulkanDevice();

/// Listas de comandos do backend: não imediatas, para várias poderem estar abertas ao
/// mesmo tempo (a interface envia geometria nova no meio do frame, por exemplo).
inline nvrhi::CommandListParameters commandListParameters() {
    return nvrhi::CommandListParameters().setEnableImmediateExecution(false);
}

/// Mensagens do NVRHI e da validação no log da engine.
nvrhi::IMessageCallback* messageCallback();

} // namespace lkz::nvrhi_detail

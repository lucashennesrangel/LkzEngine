// DirectX 12: fábrica DXGI, adaptador de alto desempenho, fila de comandos, swapchain
// "flip" na janela da SDL e o device do NVRHI por cima. Sem vsync, usa tearing quando o
// sistema permite (menor latência em monitores com VRR).

#include "GraphicsDevice.hpp"

#if defined(_WIN32)

#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <nvrhi/d3d12.h>
#include <nvrhi/validation.h>
#include <dxgi1_6.h>
#if defined(__MINGW32__)
#include <nvrhi_mingw_uuids.h> // __uuidof das interfaces do D3D12 (depois dos headers delas)
#endif
#include <spdlog/spdlog.h>

namespace lkz::nvrhi_detail {

namespace {

constexpr UINT bufferCount = 3;
constexpr DXGI_FORMAT swapFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

std::string narrow(const wchar_t* text) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(size > 0 ? size - 1 : 0), '\0');
    if (size > 1) {
        WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr, nullptr);
    }
    return result;
}

class DeviceD3D12 final : public GraphicsDevice {
public:
    ~DeviceD3D12() override { destroy(); }

    bool create(SDL_Window* window, int width, int height, bool vsync, bool debug) override {
        vsync_ = vsync;
        hwnd_ = static_cast<HWND>(
            SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
        if (hwnd_ == nullptr) {
            spdlog::error("D3D12: a janela não tem HWND");
            return false;
        }

        UINT factoryFlags = 0;
        if (debug) {
            nvrhi::RefCountPtr<ID3D12Debug> debugInterface;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugInterface)))) {
                debugInterface->EnableDebugLayer();
                factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
            } else {
                spdlog::warn("D3D12: camada de depuração indisponível (instale as Ferramentas Gráficas do Windows)");
            }
        }
        if (FAILED(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&factory_)))) {
            spdlog::error("D3D12: não foi possível criar a fábrica DXGI");
            return false;
        }

        nvrhi::RefCountPtr<IDXGIAdapter1> adapter;
        nvrhi::RefCountPtr<IDXGIFactory6> factory6;
        if (SUCCEEDED(factory_->QueryInterface(IID_PPV_ARGS(&factory6)))) {
            factory6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
        }
        if (!adapter) {
            factory_->EnumAdapters1(0, &adapter);
        }
        if (!adapter || FAILED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device_)))) {
            spdlog::error("D3D12: nenhuma placa com Direct3D 12 (nível 12_0)");
            return false;
        }
        DXGI_ADAPTER_DESC1 adapterDesc{};
        adapter->GetDesc1(&adapterDesc);
        adapterName_ = narrow(adapterDesc.Description);

        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(device_->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue_)))) {
            spdlog::error("D3D12: não foi possível criar a fila de comandos");
            return false;
        }

        nvrhi::RefCountPtr<IDXGIFactory5> factory5;
        BOOL tearing = FALSE;
        if (SUCCEEDED(factory_->QueryInterface(IID_PPV_ARGS(&factory5)))) {
            factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof tearing);
        }
        tearing_ = tearing == TRUE;

        swapDesc_.Width = static_cast<UINT>(width);
        swapDesc_.Height = static_cast<UINT>(height);
        swapDesc_.Format = swapFormat;
        swapDesc_.SampleDesc.Count = 1;
        swapDesc_.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapDesc_.BufferCount = bufferCount;
        swapDesc_.Scaling = DXGI_SCALING_STRETCH;
        swapDesc_.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swapDesc_.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        swapDesc_.Flags = tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
        nvrhi::RefCountPtr<IDXGISwapChain1> swapChain1;
        if (FAILED(factory_->CreateSwapChainForHwnd(queue_, hwnd_, &swapDesc_, nullptr, nullptr, &swapChain1)) ||
            FAILED(swapChain1->QueryInterface(IID_PPV_ARGS(&swapChain_)))) {
            spdlog::error("D3D12: não foi possível criar a swapchain");
            return false;
        }
        factory_->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER); // tela cheia é da SDL

        nvrhi::d3d12::DeviceDesc desc;
        desc.errorCB = messageCallback();
        desc.pDevice = device_;
        desc.pGraphicsCommandQueue = queue_;
        nvrhi_ = nvrhi::d3d12::createDevice(desc);
        if (debug) {
            nvrhi_ = nvrhi::validation::createValidationLayer(nvrhi_);
        }
        createBackBuffers();
        return true;
    }

    void destroy() override {
        if (nvrhi_) {
            nvrhi_->waitForIdle();
        }
        textures_.clear();
        buffers_.clear();
        nvrhi_ = nullptr;
        swapChain_ = nullptr;
        queue_ = nullptr;
        device_ = nullptr;
        factory_ = nullptr;
    }

    [[nodiscard]] nvrhi::IDevice* device() const override { return nvrhi_; }
    [[nodiscard]] const char* adapterName() const override { return adapterName_.c_str(); }

    bool acquire() override {
        index_ = swapChain_->GetCurrentBackBufferIndex();
        return true;
    }

    void present() override {
        const UINT flags = !vsync_ && tearing_ ? DXGI_PRESENT_ALLOW_TEARING : 0;
        swapChain_->Present(vsync_ ? 1 : 0, flags);
    }

    void resize(int width, int height) override {
        nvrhi_->waitForIdle();
        textures_.clear();
        buffers_.clear();
        nvrhi_->runGarbageCollection();
        swapDesc_.Width = static_cast<UINT>(width);
        swapDesc_.Height = static_cast<UINT>(height);
        if (FAILED(swapChain_->ResizeBuffers(bufferCount, swapDesc_.Width, swapDesc_.Height, swapFormat, swapDesc_.Flags))) {
            spdlog::error("D3D12: não foi possível redimensionar a swapchain");
        }
        createBackBuffers();
    }

    [[nodiscard]] nvrhi::Format format() const override { return nvrhi::Format::RGBA8_UNORM; }
    [[nodiscard]] std::uint32_t backBufferCount() const override { return static_cast<std::uint32_t>(textures_.size()); }
    [[nodiscard]] std::uint32_t backBufferIndex() const override { return index_; }
    [[nodiscard]] nvrhi::ITexture* backBuffer(std::uint32_t index) const override { return textures_[index]; }

private:
    void createBackBuffers() {
        buffers_.resize(bufferCount);
        textures_.resize(bufferCount);
        for (UINT i = 0; i < bufferCount; ++i) {
            swapChain_->GetBuffer(i, IID_PPV_ARGS(&buffers_[i]));
            nvrhi::TextureDesc desc;
            desc.width = swapDesc_.Width;
            desc.height = swapDesc_.Height;
            desc.format = nvrhi::Format::RGBA8_UNORM;
            desc.debugName = "Imagem da swapchain";
            desc.isRenderTarget = true;
            desc.initialState = nvrhi::ResourceStates::Present;
            desc.keepInitialState = true;
            textures_[i] = nvrhi_->createHandleForNativeTexture(nvrhi::ObjectTypes::D3D12_Resource,
                                                                nvrhi::Object(buffers_[i].Get()), desc);
        }
    }

    HWND hwnd_ = nullptr;
    bool vsync_ = true;
    bool tearing_ = false;
    std::string adapterName_;
    nvrhi::RefCountPtr<IDXGIFactory2> factory_;
    nvrhi::RefCountPtr<ID3D12Device> device_;
    nvrhi::RefCountPtr<ID3D12CommandQueue> queue_;
    nvrhi::RefCountPtr<IDXGISwapChain3> swapChain_;
    DXGI_SWAP_CHAIN_DESC1 swapDesc_{};
    std::vector<nvrhi::RefCountPtr<ID3D12Resource>> buffers_;
    std::vector<nvrhi::TextureHandle> textures_;
    nvrhi::DeviceHandle nvrhi_;
    std::uint32_t index_ = 0;
};

} // namespace

std::unique_ptr<GraphicsDevice> createD3D12Device() {
    return std::make_unique<DeviceD3D12>();
}

} // namespace lkz::nvrhi_detail

#else

namespace lkz::nvrhi_detail {

std::unique_ptr<GraphicsDevice> createD3D12Device() {
    return nullptr; // DirectX 12 só existe no Windows
}

} // namespace lkz::nvrhi_detail

#endif

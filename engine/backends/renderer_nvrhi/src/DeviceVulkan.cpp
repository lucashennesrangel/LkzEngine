// Vulkan 1.3: instância (com as extensões de janela da SDL), superfície da janela, a
// placa com fila de gráficos que apresenta nela (dedicada primeiro), device com os
// recursos que o NVRHI usa (renderização dinâmica, synchronization2, semáforos de linha
// do tempo), swapchain e o device do NVRHI por cima.
//
// As funções do Vulkan são buscadas na hora (vkGetInstanceProcAddr da SDL): basta o
// driver da placa, sem SDK instalado.

#include "GraphicsDevice.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <nvrhi/validation.h>
#include <nvrhi/vulkan.h>
#include <spdlog/spdlog.h>

namespace lkz::nvrhi_detail {

void initVulkanDispatch(VkInstance instance, PFN_vkGetInstanceProcAddr getInstanceProcAddr, VkDevice device); // VulkanDispatch.cpp

namespace {

/// Struct do Vulkan zerada, com o tipo preenchido.
template <class T>
T make(VkStructureType type) {
    T value{};
    value.sType = type;
    return value;
}

// Funções usadas aqui (o NVRHI carrega as dele).
#define LKZ_VK_INSTANCE_FUNCTIONS(X)                                                                                   \
    X(vkDestroyInstance)                                                                                               \
    X(vkEnumeratePhysicalDevices)                                                                                      \
    X(vkGetPhysicalDeviceProperties)                                                                                   \
    X(vkGetPhysicalDeviceFeatures)                                                                                     \
    X(vkGetPhysicalDeviceQueueFamilyProperties)                                                                        \
    X(vkGetPhysicalDeviceSurfaceSupportKHR)                                                                            \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)                                                                       \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR)                                                                            \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR)                                                                       \
    X(vkEnumerateDeviceExtensionProperties)                                                                            \
    X(vkCreateDevice)                                                                                                  \
    X(vkGetDeviceProcAddr)                                                                                             \
    X(vkDestroySurfaceKHR)
#define LKZ_VK_DEVICE_FUNCTIONS(X)                                                                                     \
    X(vkDestroyDevice)                                                                                                 \
    X(vkGetDeviceQueue)                                                                                                \
    X(vkDeviceWaitIdle)                                                                                                \
    X(vkCreateSwapchainKHR)                                                                                            \
    X(vkDestroySwapchainKHR)                                                                                           \
    X(vkGetSwapchainImagesKHR)                                                                                         \
    X(vkAcquireNextImageKHR)                                                                                           \
    X(vkQueuePresentKHR)                                                                                               \
    X(vkCreateSemaphore)                                                                                               \
    X(vkDestroySemaphore)

class DeviceVulkan final : public GraphicsDevice {
public:
    ~DeviceVulkan() override { destroy(); }

    bool create(SDL_Window* window, int width, int height, bool vsync, bool debug) override {
        vsync_ = vsync;
        getInstanceProcAddr_ = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
        if (getInstanceProcAddr_ == nullptr) {
            spdlog::error("Vulkan: biblioteca não encontrada ({})", SDL_GetError());
            return false;
        }
        const auto createInstance =
            reinterpret_cast<PFN_vkCreateInstance>(getInstanceProcAddr_(VK_NULL_HANDLE, "vkCreateInstance"));
        const auto enumerateVersion = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
            getInstanceProcAddr_(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
        const auto enumerateLayers = reinterpret_cast<PFN_vkEnumerateInstanceLayerProperties>(
            getInstanceProcAddr_(VK_NULL_HANDLE, "vkEnumerateInstanceLayerProperties"));
        std::uint32_t version = VK_API_VERSION_1_0;
        if (enumerateVersion != nullptr) {
            enumerateVersion(&version);
        }
        if (createInstance == nullptr || version < VK_API_VERSION_1_3) {
            spdlog::error("Vulkan: precisa da versão 1.3 (o driver oferece {}.{})", VK_API_VERSION_MAJOR(version),
                          VK_API_VERSION_MINOR(version));
            return false;
        }

        // --- Instância ---
        Uint32 windowExtensionCount = 0;
        const char* const* windowExtensions = SDL_Vulkan_GetInstanceExtensions(&windowExtensionCount);
        instanceExtensions_.assign(windowExtensions, windowExtensions + windowExtensionCount);
        std::vector<const char*> layers;
        if (debug) {
            std::uint32_t count = 0;
            enumerateLayers(&count, nullptr);
            std::vector<VkLayerProperties> available(count);
            enumerateLayers(&count, available.data());
            const bool found = std::any_of(available.begin(), available.end(), [](const VkLayerProperties& layer) {
                return std::strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0;
            });
            if (found) {
                layers.push_back("VK_LAYER_KHRONOS_validation");
            } else {
                spdlog::warn("Vulkan: camada de validação indisponível (instale o Vulkan SDK)");
            }
        }
        auto application = make<VkApplicationInfo>(VK_STRUCTURE_TYPE_APPLICATION_INFO);
        application.pApplicationName = "Lkz Engine";
        application.pEngineName = "Lkz Engine";
        application.apiVersion = VK_API_VERSION_1_3;
        auto instanceInfo = make<VkInstanceCreateInfo>(VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);
        instanceInfo.pApplicationInfo = &application;
        instanceInfo.enabledExtensionCount = static_cast<std::uint32_t>(instanceExtensions_.size());
        instanceInfo.ppEnabledExtensionNames = instanceExtensions_.data();
        instanceInfo.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
        instanceInfo.ppEnabledLayerNames = layers.data();
        if (createInstance(&instanceInfo, nullptr, &instance_) != VK_SUCCESS) {
            spdlog::error("Vulkan: não foi possível criar a instância");
            return false;
        }
#define LKZ_LOAD(name) name = reinterpret_cast<PFN_##name>(getInstanceProcAddr_(instance_, #name));
        LKZ_VK_INSTANCE_FUNCTIONS(LKZ_LOAD)
#undef LKZ_LOAD

        if (!SDL_Vulkan_CreateSurface(window, instance_, nullptr, &surface_)) {
            spdlog::error("Vulkan: não foi possível criar a superfície da janela ({})", SDL_GetError());
            return false;
        }

        // --- Placa e fila ---
        if (!choosePhysicalDevice()) {
            spdlog::error("Vulkan: nenhuma placa com Vulkan 1.3 que desenhe nesta janela");
            return false;
        }

        // --- Device ---
        VkPhysicalDeviceFeatures supported{};
        vkGetPhysicalDeviceFeatures(physicalDevice_, &supported);
        auto features13 = make<VkPhysicalDeviceVulkan13Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES);
        features13.dynamicRendering = VK_TRUE;
        features13.synchronization2 = VK_TRUE;
        features13.maintenance4 = VK_TRUE;
        auto features12 = make<VkPhysicalDeviceVulkan12Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
        features12.pNext = &features13;
        features12.timelineSemaphore = VK_TRUE;
        auto features = make<VkPhysicalDeviceFeatures2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
        features.pNext = &features12;
        features.features.samplerAnisotropy = supported.samplerAnisotropy;
        features.features.fillModeNonSolid = supported.fillModeNonSolid;

        const float priority = 1.0f;
        auto queueInfo = make<VkDeviceQueueCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO);
        queueInfo.queueFamilyIndex = queueFamily_;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;
        deviceExtensions_ = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        auto deviceInfo = make<VkDeviceCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);
        deviceInfo.pNext = &features;
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        deviceInfo.enabledExtensionCount = static_cast<std::uint32_t>(deviceExtensions_.size());
        deviceInfo.ppEnabledExtensionNames = deviceExtensions_.data();
        if (vkCreateDevice(physicalDevice_, &deviceInfo, nullptr, &device_) != VK_SUCCESS) {
            spdlog::error("Vulkan: não foi possível criar o device");
            return false;
        }
#define LKZ_LOAD(name) name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device_, #name));
        LKZ_VK_DEVICE_FUNCTIONS(LKZ_LOAD)
#undef LKZ_LOAD
        vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);

        initVulkanDispatch(instance_, getInstanceProcAddr_, device_);
        nvrhi::vulkan::DeviceDesc desc;
        desc.errorCB = messageCallback();
        desc.instance = instance_;
        desc.physicalDevice = physicalDevice_;
        desc.device = device_;
        desc.graphicsQueue = queue_;
        desc.graphicsQueueIndex = static_cast<int>(queueFamily_);
        desc.instanceExtensions = instanceExtensions_.data();
        desc.numInstanceExtensions = instanceExtensions_.size();
        desc.deviceExtensions = deviceExtensions_.data();
        desc.numDeviceExtensions = deviceExtensions_.size();
        vulkanDevice_ = nvrhi::vulkan::createDevice(desc);
        if (!vulkanDevice_) {
            spdlog::error("Vulkan: o NVRHI não aceitou o device");
            return false;
        }
        nvrhi_ = debug ? nvrhi::validation::createValidationLayer(vulkanDevice_) : nvrhi::DeviceHandle{vulkanDevice_};

        return createSwapchain(width, height);
    }

    void destroy() override {
        if (device_ != VK_NULL_HANDLE) {
            if (nvrhi_) {
                nvrhi_->waitForIdle();
            }
            destroySwapchain();
            nvrhi_ = nullptr;
            vulkanDevice_ = nullptr;
            vkDestroyDevice(device_, nullptr);
            device_ = VK_NULL_HANDLE;
        }
        if (surface_ != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(instance_, surface_, nullptr);
            surface_ = VK_NULL_HANDLE;
        }
        if (instance_ != VK_NULL_HANDLE) {
            vkDestroyInstance(instance_, nullptr);
            instance_ = VK_NULL_HANDLE;
        }
    }

    [[nodiscard]] nvrhi::IDevice* device() const override { return nvrhi_; }
    [[nodiscard]] const char* adapterName() const override { return adapterName_.c_str(); }

    bool acquire() override {
        if (swapchain_ == VK_NULL_HANDLE) {
            return false; // minimizada: a swapchain volta no resize
        }
        const VkSemaphore semaphore = acquireSemaphores_[acquireIndex_];
        const VkResult result = vkAcquireNextImageKHR(device_, swapchain_, std::numeric_limits<std::uint64_t>::max(),
                                                      semaphore, VK_NULL_HANDLE, &imageIndex_);
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
            return false; // VK_ERROR_OUT_OF_DATE_KHR: a janela mudou de tamanho
        }
        acquireIndex_ = (acquireIndex_ + 1) % static_cast<std::uint32_t>(acquireSemaphores_.size());
        vulkanDevice_->queueWaitForSemaphore(nvrhi::CommandQueue::Graphics, semaphore, 0);
        return true;
    }

    void present() override {
        // O NVRHI só sinaliza o semáforo na próxima submissão: a lista vazia faz isso agora.
        const VkSemaphore semaphore = presentSemaphores_[imageIndex_];
        vulkanDevice_->queueSignalSemaphore(nvrhi::CommandQueue::Graphics, semaphore, 0);
        nvrhi_->executeCommandLists(nullptr, 0);
        auto info = make<VkPresentInfoKHR>(VK_STRUCTURE_TYPE_PRESENT_INFO_KHR);
        info.waitSemaphoreCount = 1;
        info.pWaitSemaphores = &semaphore;
        info.swapchainCount = 1;
        info.pSwapchains = &swapchain_;
        info.pImageIndices = &imageIndex_;
        vkQueuePresentKHR(queue_, &info);
    }

    void resize(int width, int height) override {
        nvrhi_->waitForIdle();
        vkDeviceWaitIdle(device_);
        destroySwapchain();
        nvrhi_->runGarbageCollection();
        createSwapchain(width, height);
    }

    [[nodiscard]] nvrhi::Format format() const override { return format_; }
    [[nodiscard]] std::uint32_t backBufferCount() const override { return static_cast<std::uint32_t>(textures_.size()); }
    [[nodiscard]] std::uint32_t backBufferIndex() const override { return imageIndex_; }
    [[nodiscard]] nvrhi::ITexture* backBuffer(std::uint32_t index) const override { return textures_[index]; }

private:
    bool choosePhysicalDevice() {
        std::uint32_t count = 0;
        vkEnumeratePhysicalDevices(instance_, &count, nullptr);
        std::vector<VkPhysicalDevice> devices(count);
        vkEnumeratePhysicalDevices(instance_, &count, devices.data());
        int bestScore = -1;
        for (VkPhysicalDevice candidate : devices) {
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(candidate, &properties);
            if (properties.apiVersion < VK_API_VERSION_1_3 || !hasSwapchain(candidate)) {
                continue;
            }
            std::uint32_t familyCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, nullptr);
            std::vector<VkQueueFamilyProperties> families(familyCount);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, families.data());
            for (std::uint32_t family = 0; family < familyCount; ++family) {
                VkBool32 present = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(candidate, family, surface_, &present);
                if ((families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0 || present != VK_TRUE) {
                    continue;
                }
                const int score = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? 2
                                  : properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 1
                                                                                                    : 0;
                if (score > bestScore) {
                    bestScore = score;
                    physicalDevice_ = candidate;
                    queueFamily_ = family;
                    adapterName_ = properties.deviceName;
                }
                break;
            }
        }
        return bestScore >= 0;
    }

    bool hasSwapchain(VkPhysicalDevice candidate) const {
        std::uint32_t count = 0;
        vkEnumerateDeviceExtensionProperties(candidate, nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> extensions(count);
        vkEnumerateDeviceExtensionProperties(candidate, nullptr, &count, extensions.data());
        return std::any_of(extensions.begin(), extensions.end(), [](const VkExtensionProperties& extension) {
            return std::strcmp(extension.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0;
        });
    }

    bool createSwapchain(int width, int height) {
        VkSurfaceCapabilitiesKHR capabilities{};
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice_, surface_, &capabilities);
        std::uint32_t formatCount = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &formatCount, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(formatCount);
        vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &formatCount, formats.data());
        // BGRA de 8 bits sem conversão sRGB: as mesmas cores do backend raylib.
        VkSurfaceFormatKHR format{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
        format_ = nvrhi::Format::BGRA8_UNORM;
        const bool hasBgra = std::any_of(formats.begin(), formats.end(), [](const VkSurfaceFormatKHR& f) {
            return f.format == VK_FORMAT_B8G8R8A8_UNORM;
        });
        if (!hasBgra) {
            format.format = VK_FORMAT_R8G8B8A8_UNORM;
            format_ = nvrhi::Format::RGBA8_UNORM;
        }

        std::uint32_t modeCount = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice_, surface_, &modeCount, nullptr);
        std::vector<VkPresentModeKHR> modes(modeCount);
        vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice_, surface_, &modeCount, modes.data());
        VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR; // vsync, sempre disponível
        if (!vsync_) {
            for (const VkPresentModeKHR preferred : {VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_MAILBOX_KHR}) {
                if (std::find(modes.begin(), modes.end(), preferred) != modes.end()) {
                    mode = preferred;
                    break;
                }
            }
        }

        VkExtent2D extent = capabilities.currentExtent;
        if (extent.width == std::numeric_limits<std::uint32_t>::max()) {
            extent = {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)};
        }
        extent.width = std::clamp(extent.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
        extent.height = std::clamp(extent.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
        if (extent.width == 0 || extent.height == 0) {
            return true; // janela minimizada: o Vulkan não cria swapchain de tamanho 0
        }
        std::uint32_t imageCount = std::max(capabilities.minImageCount + 1, 3u);
        if (capabilities.maxImageCount > 0) {
            imageCount = std::min(imageCount, capabilities.maxImageCount);
        }

        auto info = make<VkSwapchainCreateInfoKHR>(VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR);
        info.surface = surface_;
        info.minImageCount = imageCount;
        info.imageFormat = format.format;
        info.imageColorSpace = format.colorSpace;
        info.imageExtent = extent;
        info.imageArrayLayers = 1;
        info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.preTransform = capabilities.currentTransform;
        info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        info.presentMode = mode;
        info.clipped = VK_TRUE;
        if (vkCreateSwapchainKHR(device_, &info, nullptr, &swapchain_) != VK_SUCCESS) {
            spdlog::error("Vulkan: não foi possível criar a swapchain");
            return false;
        }

        std::uint32_t count = 0;
        vkGetSwapchainImagesKHR(device_, swapchain_, &count, nullptr);
        std::vector<VkImage> images(count);
        vkGetSwapchainImagesKHR(device_, swapchain_, &count, images.data());
        textures_.clear();
        for (VkImage image : images) {
            nvrhi::TextureDesc desc;
            desc.width = extent.width;
            desc.height = extent.height;
            desc.format = format_;
            desc.debugName = "Imagem da swapchain";
            desc.isRenderTarget = true;
            desc.initialState = nvrhi::ResourceStates::Present;
            desc.keepInitialState = true;
            textures_.push_back(nvrhi_->createHandleForNativeTexture(nvrhi::ObjectTypes::VK_Image, nvrhi::Object(image), desc));
        }

        // Um semáforo de "imagem pronta" por frame em voo e um de "pode apresentar" por imagem.
        const auto semaphoreInfo = make<VkSemaphoreCreateInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO);
        acquireSemaphores_.resize(std::max<std::size_t>(count, 3));
        presentSemaphores_.resize(count);
        for (VkSemaphore& semaphore : acquireSemaphores_) {
            vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &semaphore);
        }
        for (VkSemaphore& semaphore : presentSemaphores_) {
            vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &semaphore);
        }
        acquireIndex_ = 0;
        return true;
    }

    void destroySwapchain() {
        textures_.clear();
        for (VkSemaphore semaphore : acquireSemaphores_) {
            vkDestroySemaphore(device_, semaphore, nullptr);
        }
        for (VkSemaphore semaphore : presentSemaphores_) {
            vkDestroySemaphore(device_, semaphore, nullptr);
        }
        acquireSemaphores_.clear();
        presentSemaphores_.clear();
        if (swapchain_ != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(device_, swapchain_, nullptr);
            swapchain_ = VK_NULL_HANDLE;
        }
    }

    PFN_vkGetInstanceProcAddr getInstanceProcAddr_ = nullptr;
#define LKZ_DECLARE(name) PFN_##name name = nullptr;
    LKZ_VK_INSTANCE_FUNCTIONS(LKZ_DECLARE)
    LKZ_VK_DEVICE_FUNCTIONS(LKZ_DECLARE)
#undef LKZ_DECLARE

    bool vsync_ = true;
    nvrhi::Format format_ = nvrhi::Format::BGRA8_UNORM;
    std::string adapterName_;
    std::vector<const char*> instanceExtensions_;
    std::vector<const char*> deviceExtensions_;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    std::uint32_t queueFamily_ = 0;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    std::vector<nvrhi::TextureHandle> textures_;
    std::vector<VkSemaphore> acquireSemaphores_;
    std::vector<VkSemaphore> presentSemaphores_;
    std::uint32_t acquireIndex_ = 0;
    std::uint32_t imageIndex_ = 0;
    nvrhi::vulkan::DeviceHandle vulkanDevice_;
    nvrhi::DeviceHandle nvrhi_;
};

} // namespace

std::unique_ptr<GraphicsDevice> createVulkanDevice() {
    return std::make_unique<DeviceVulkan>();
}

} // namespace lkz::nvrhi_detail

// O despachante dinâmico do vulkan.hpp, que o backend Vulkan do NVRHI usa. Numa
// biblioteca estática o NVRHI não o define nem o inicializa: o programa faz as duas
// coisas (em um só arquivo, antes de nvrhi::vulkan::createDevice).
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <vulkan/vulkan.hpp>

VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

namespace lkz::nvrhi_detail {

void initVulkanDispatch(VkInstance instance, PFN_vkGetInstanceProcAddr getInstanceProcAddr, VkDevice device) {
    VULKAN_HPP_DEFAULT_DISPATCHER.init(instance, getInstanceProcAddr, device);
}

} // namespace lkz::nvrhi_detail

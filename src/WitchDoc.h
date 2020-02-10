#pragma once

#include <vulkan/vulkan.h>

struct LayerBypassDispatch {
    // instance functions, used for layer-managed query pool setup
    PFN_vkGetPhysicalDeviceProperties getPhysicalDeviceProperties;
};

class WitchDoctor {
public:
    WitchDoctor();
    ~WitchDoctor();

    VkResult PostCallCreateInstance(const VkInstanceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkInstance* pInstance);
    VkResult PostCallCreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo * pCreateInfo, const VkAllocationCallbacks * pAllocator, VkDevice * pDevice);

protected:
    PFN_vkVoidFunction GetDeviceProcAddr_DispatchHelper(const char *pName);
    PFN_vkVoidFunction GetInstanceProcAddr_DispatchHelper(const char *pName);

    void PopulateInstanceLayerBypassDispatchTable();
    void PopulateDeviceLayerBypassDispatchTable();

private:
    LayerBypassDispatch m_layerBypassDispatch = {};

    VkInstance m_instance = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;

};
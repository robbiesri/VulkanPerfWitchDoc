#include "WitchDoc.h"
#include "layerCore.h"

WitchDoctor::WitchDoctor() {}

WitchDoctor::~WitchDoctor() {}

PFN_vkVoidFunction WitchDoctor::GetDeviceProcAddr_DispatchHelper(
    const char* pName) {
  return GWD::GwdGetDispatchedDeviceProcAddr(m_device, pName);
}

PFN_vkVoidFunction WitchDoctor::GetInstanceProcAddr_DispatchHelper(
    const char* pName) {
  return GWD::GwdGetDispatchedInstanceProcAddr(m_instance, pName);
}

void WitchDoctor::PopulateInstanceLayerBypassDispatchTable() {
  m_layerBypassDispatch.getPhysicalDeviceProperties =
      (PFN_vkGetPhysicalDeviceProperties)GetInstanceProcAddr_DispatchHelper(
          "vkGetPhysicalDeviceProperties");
  m_layerBypassDispatch.getPhysicalDeviceMemoryProperties =
      (PFN_vkGetPhysicalDeviceMemoryProperties)
          GetInstanceProcAddr_DispatchHelper(
              "vkGetPhysicalDeviceMemoryProperties");
}

void WitchDoctor::PopulateDeviceLayerBypassDispatchTable() {}

VkResult WitchDoctor::PostCallCreateInstance(
    const VkInstanceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator, VkInstance* pInstance) {
  m_instance = *pInstance;
  PopulateInstanceLayerBypassDispatchTable();

  return VK_SUCCESS;
}

VkResult WitchDoctor::PostCallCreateDevice(
    VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator, VkDevice* pDevice) {
  m_device = *pDevice;
  PopulateDeviceLayerBypassDispatchTable();

  m_layerBypassDispatch.getPhysicalDeviceMemoryProperties(physicalDevice,
                                                          &m_physDevMemProps);

  return VK_SUCCESS;
}

VkResult WitchDoctor::PostCallAllocateMemory(
    const VkResult inResult, VkDevice device,
    const VkMemoryAllocateInfo* pAllocateInfo,
    const VkAllocationCallbacks* pAllocator, VkDeviceMemory* pMemory) {
  if (inResult != VK_SUCCESS) {
    return inResult;
  }

  m_allocToMemTypeMap[*pMemory] = pAllocateInfo->memoryTypeIndex;

  return VK_SUCCESS;
}

void WitchDoctor::PostCallFreeMemory(VkDevice device, VkDeviceMemory memory,
                                     const VkAllocationCallbacks* pAllocator) {
  if (m_allocToMemTypeMap.find(memory) != m_allocToMemTypeMap.end()) {
    m_allocToMemTypeMap[memory] = UINT32_MAX;
  }
}
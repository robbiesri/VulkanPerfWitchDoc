#include "WitchDoc.h"
#include "layerCore.h"

namespace GWD {

PFN_vkVoidFunction WitchDoctor::GetDeviceProcAddr_DispatchHelper(
    const char* pName) {
  return GWDInterface::GwdGetDispatchedDeviceProcAddr(m_device, pName);
}

PFN_vkVoidFunction WitchDoctor::GetInstanceProcAddr_DispatchHelper(
    const char* pName) {
  return GWDInterface::GwdGetDispatchedInstanceProcAddr(m_instance, pName);
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

VkResult WitchDoctor::PostCallBindBufferMemory(const VkResult inResult,
                                               VkDevice device, VkBuffer buffer,
                                               VkDeviceMemory memory,
                                               VkDeviceSize memoryOffset) {
  if (inResult != VK_SUCCESS) {
    return inResult;
  }

  if (m_bufferToMemTypeMap.find(buffer) != m_bufferToMemTypeMap.end()) {
    m_bufferToMemTypeMap[buffer] = m_allocToMemTypeMap[memory];
  }

  return VK_SUCCESS;
}

VkResult WitchDoctor::PostCallCreateBuffer(
    const VkResult inResult, VkDevice device,
    const VkBufferCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator, VkBuffer* pBuffer) {
  if (inResult != VK_SUCCESS) {
    return inResult;
  }

  if ((pCreateInfo->usage & (VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                             VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) != 0) {
    m_bufferToMemTypeMap[*pBuffer] = UINT32_MAX;
  }

  return VK_SUCCESS;
}

void WitchDoctor::PostCallDestroyBuffer(
    VkDevice device, VkBuffer buffer, const VkAllocationCallbacks* pAllocator) {
  if (m_bufferToMemTypeMap.find(buffer) != m_bufferToMemTypeMap.end()) {
    m_bufferToMemTypeMap.erase(buffer);
  }
}

VkResult WitchDoctor::PostCallBindBufferMemory2(
    const VkResult inResult, VkDevice device, uint32_t bindInfoCount,
    const VkBindBufferMemoryInfo* pBindInfos) {
  if (inResult != VK_SUCCESS) {
    return inResult;
  }

  return VK_SUCCESS;
}

// TODO: Match at BindVertexBuffer and BindIndexBuffer time
// Defer to draw?
// TODO: What about compute buffers?

}  // namespace GWD

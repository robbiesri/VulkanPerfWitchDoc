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

  // TODO: implement

  return VK_SUCCESS;
}

void WitchDoctor::PostCallCmdDraw(VkCommandBuffer commandBuffer,
                                  uint32_t vertexCount, uint32_t instanceCount,
                                  uint32_t firstVertex,
                                  uint32_t firstInstance) {}

void WitchDoctor::PostCallCmdDrawIndexed(
    VkCommandBuffer commandBuffer, uint32_t indexCount, uint32_t instanceCount,
    uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) {}
void WitchDoctor::PostCallCmdDrawIndirect(VkCommandBuffer commandBuffer,
                                          VkBuffer buffer, VkDeviceSize offset,
                                          uint32_t drawCount, uint32_t stride) {
}
void WitchDoctor::PostCallCmdDrawIndexedIndirect(VkCommandBuffer commandBuffer,
                                                 VkBuffer buffer,
                                                 VkDeviceSize offset,
                                                 uint32_t drawCount,
                                                 uint32_t stride) {}

void WitchDoctor::PostCallCmdBindIndexBuffer(VkCommandBuffer commandBuffer,
                                             VkBuffer buffer,
                                             VkDeviceSize offset,
                                             VkIndexType indexType) {}

void WitchDoctor::PostCallCmdBindVertexBuffers(VkCommandBuffer commandBuffer,
                                               uint32_t firstBinding,
                                               uint32_t bindingCount,
                                               const VkBuffer* pBuffers,
                                               const VkDeviceSize* pOffsets) {}

// TODO: Match at BindVertexBuffer and BindIndexBuffer time
// TODO: What about compute buffers?

}  // namespace GWD

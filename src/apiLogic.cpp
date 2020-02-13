/*
 Copyright 2020 Google Inc.

 Licensed under the Apache License, Version 2.0 (the "License");
 you may not use this file except in compliance with the License.
 You may obtain a copy of the License at

 http://www.apache.org/licenses/LICENSE-2.0

 Unless required by applicable law or agreed to in writing, software
 distributed under the License is distributed on an "AS IS" BASIS,
 WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 See the License for the specific language governing permissions and
 limitations under the License.
*/

#include "WitchDoc.h"
#include "layerCore.h"

#include <iostream>
#include <sstream>

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

VkResult WitchDoctor::PostCallCreateDebugUtilsMessengerEXT(
    const VkResult inResult, VkInstance instance,
    VkDebugUtilsMessengerCreateInfoEXT const* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDebugUtilsMessengerEXT* pMessenger) {
  if (VK_SUCCESS != inResult) {
    return inResult;
  }

  std::lock_guard<std::mutex> lock(m_debug_utils_messenger_mutex);
  m_debug_utils_messengers.emplace(*pMessenger, *pCreateInfo);

  return VK_SUCCESS;
}

void WitchDoctor::PostCallDestroyDebugUtilsMessengerEXT(
    VkInstance instance, VkDebugUtilsMessengerEXT messenger,
    const VkAllocationCallbacks* pAllocator) {
  std::lock_guard<std::mutex> lock(m_debug_utils_messenger_mutex);
  m_debug_utils_messengers.erase(messenger);
}

VkResult WitchDoctor::PostCallCreateDevice(
    VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator, VkDevice* pDevice) {
  m_device = *pDevice;
  PopulateDeviceLayerBypassDispatchTable();

  m_layerBypassDispatch.getPhysicalDeviceMemoryProperties(physicalDevice,
                                                          &m_physDevMemProps);

  m_memTypeIsDeviceLocal.resize(m_physDevMemProps.memoryTypeCount);
  for (uint32_t mem_type_index = 0;
       mem_type_index < m_physDevMemProps.memoryTypeCount; mem_type_index++) {
    m_memTypeIsDeviceLocal[mem_type_index] =
        ((m_physDevMemProps.memoryTypes[mem_type_index].propertyFlags &
          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0);
  }

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

// TODO: Report through debug_utils or stderr

void WitchDoctor::PostCallCmdDraw(VkCommandBuffer commandBuffer,
                                  uint32_t vertexCount, uint32_t instanceCount,
                                  uint32_t firstVertex,
                                  uint32_t firstInstance) {
  if (!m_vertex_buffers_are_device_local) {
    //std::cout << "vkCmdDraw is using vertex buffers that are not DEVICE_LOCAL"
    //          << std::endl;

    std::ostringstream warn_log;
    warn_log << "vkCmdDraw is using vertex buffers that are not DEVICE_LOCAL";
    PerformanceWarningMessage(warn_log.str());
  }
}

void WitchDoctor::PostCallCmdDrawIndexed(
    VkCommandBuffer commandBuffer, uint32_t indexCount, uint32_t instanceCount,
    uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) {
  if (!m_index_buffer_is_device_local) {
    //std::cout
    //    << "vkCmdDrawIndexed is using index buffer that is not DEVICE_LOCAL"
    //    << std::endl;
    std::ostringstream warn_log;
    warn_log
        << "vkCmdDrawIndexed is using index buffer that is not DEVICE_LOCAL";
    PerformanceWarningMessage(warn_log.str());
  }

  if (!m_vertex_buffers_are_device_local) {
    std::cout << "vkCmdDraw is using vertex buffers that are not DEVICE_LOCAL"
              << std::endl;
  }
}

void WitchDoctor::PostCallCmdDrawIndirect(VkCommandBuffer commandBuffer,
                                          VkBuffer buffer, VkDeviceSize offset,
                                          uint32_t drawCount, uint32_t stride) {
  if (!m_vertex_buffers_are_device_local) {
    std::cout
        << "vkCmdDrawIndirect is using vertex buffers that are not DEVICE_LOCAL"
        << std::endl;
  }
}
void WitchDoctor::PostCallCmdDrawIndexedIndirect(VkCommandBuffer commandBuffer,
                                                 VkBuffer buffer,
                                                 VkDeviceSize offset,
                                                 uint32_t drawCount,
                                                 uint32_t stride) {
  if (!m_index_buffer_is_device_local) {
    std::cout << "vkCmdDrawIndexedIndirect is using index buffer that is not "
                 "DEVICE_LOCAL"
              << std::endl;
  }

  if (!m_vertex_buffers_are_device_local) {
    std::cout << "vkCmdDrawIndexedIndirect is using vertex buffers that are "
                 "not DEVICE_LOCAL"
              << std::endl;
  }
}

void WitchDoctor::PostCallCmdBindIndexBuffer(VkCommandBuffer commandBuffer,
                                             VkBuffer buffer,
                                             VkDeviceSize offset,
                                             VkIndexType indexType) {
  // TODO: Monitor for using VK_INDEX_TYPE_UINT32 if they don't have large index
  // counts

  uint32_t mem_type_index = m_bufferToMemTypeMap[buffer];
  m_index_buffer_is_device_local =
      (m_memTypeIsDeviceLocal[mem_type_index] == true);
}

void WitchDoctor::PostCallCmdBindVertexBuffers(VkCommandBuffer commandBuffer,
                                               uint32_t firstBinding,
                                               uint32_t bindingCount,
                                               const VkBuffer* pBuffers,
                                               const VkDeviceSize* pOffsets) {
  bool all_buffers_device_local = true;
  for (uint32_t buffer_index = 0; buffer_index < bindingCount; buffer_index++) {
    VkBuffer buffer = pBuffers[buffer_index];
    uint32_t mem_type_index = m_bufferToMemTypeMap[buffer];
    if (m_memTypeIsDeviceLocal[mem_type_index] == false) {
      all_buffers_device_local = false;
      break;
    }
  }
  m_vertex_buffers_are_device_local = all_buffers_device_local;
}

// TODO: What about compute buffers?

}  // namespace GWD

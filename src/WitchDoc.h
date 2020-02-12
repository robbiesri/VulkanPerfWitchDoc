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

#pragma once

#define NOMINMAX

#include <vulkan/vulkan.h>

#include "flat_hash_map.hpp"
#include <mutex>
#include <vector>

namespace GWD {

struct LayerBypassDispatch {
  // instance functions, used for layer-managed query pool setup
  PFN_vkGetPhysicalDeviceProperties getPhysicalDeviceProperties;
  PFN_vkGetPhysicalDeviceMemoryProperties getPhysicalDeviceMemoryProperties;
};

class WitchDoctor {
 public:
  WitchDoctor();
  ~WitchDoctor();

  VkResult PostCallCreateInstance(const VkInstanceCreateInfo* pCreateInfo,
                                  const VkAllocationCallbacks* pAllocator,
                                  VkInstance* pInstance);
  VkResult PostCallCreateDebugUtilsMessengerEXT(
      const VkResult inResult, VkInstance instance,
      VkDebugUtilsMessengerCreateInfoEXT const* pCreateInfo,
      const VkAllocationCallbacks* pAllocator,
      VkDebugUtilsMessengerEXT* pMessenger);
  void PostCallDestroyDebugUtilsMessengerEXT(
      VkInstance instance, VkDebugUtilsMessengerEXT messenger,
      const VkAllocationCallbacks* pAllocator);
  VkResult PostCallCreateDevice(VkPhysicalDevice physicalDevice,
                                const VkDeviceCreateInfo* pCreateInfo,
                                const VkAllocationCallbacks* pAllocator,
                                VkDevice* pDevice);
  VkResult PostCallAllocateMemory(const VkResult inResult, VkDevice device,
                                  const VkMemoryAllocateInfo* pAllocateInfo,
                                  const VkAllocationCallbacks* pAllocator,
                                  VkDeviceMemory* pMemory);
  void PostCallFreeMemory(VkDevice device, VkDeviceMemory memory,
                          const VkAllocationCallbacks* pAllocator);
  VkResult PostCallBindBufferMemory(const VkResult inResult, VkDevice device,
                                    VkBuffer buffer, VkDeviceMemory memory,
                                    VkDeviceSize memoryOffset);
  VkResult PostCallCreateBuffer(const VkResult inResult, VkDevice device,
                                const VkBufferCreateInfo* pCreateInfo,
                                const VkAllocationCallbacks* pAllocator,
                                VkBuffer* pBuffer);
  void PostCallDestroyBuffer(VkDevice device, VkBuffer buffer,
                             const VkAllocationCallbacks* pAllocator);
  VkResult PostCallBindBufferMemory2(const VkResult inResult, VkDevice device,
                                     uint32_t bindInfoCount,
                                     const VkBindBufferMemoryInfo* pBindInfos);
  void PostCallCmdDraw(VkCommandBuffer commandBuffer, uint32_t vertexCount,
                       uint32_t instanceCount, uint32_t firstVertex,
                       uint32_t firstInstance);
  void PostCallCmdDrawIndexed(VkCommandBuffer commandBuffer,
                              uint32_t indexCount, uint32_t instanceCount,
                              uint32_t firstIndex, int32_t vertexOffset,
                              uint32_t firstInstance);
  void PostCallCmdDrawIndirect(VkCommandBuffer commandBuffer, VkBuffer buffer,
                               VkDeviceSize offset, uint32_t drawCount,
                               uint32_t stride);
  void PostCallCmdDrawIndexedIndirect(VkCommandBuffer commandBuffer,
                                      VkBuffer buffer, VkDeviceSize offset,
                                      uint32_t drawCount, uint32_t stride);
  void PostCallCmdBindIndexBuffer(VkCommandBuffer commandBuffer,
                                  VkBuffer buffer, VkDeviceSize offset,
                                  VkIndexType indexType);
  void PostCallCmdBindVertexBuffers(VkCommandBuffer commandBuffer,
                                    uint32_t firstBinding,
                                    uint32_t bindingCount,
                                    const VkBuffer* pBuffers,
                                    const VkDeviceSize* pOffsets);

 protected:
  PFN_vkVoidFunction GetDeviceProcAddr_DispatchHelper(const char* pName);
  PFN_vkVoidFunction GetInstanceProcAddr_DispatchHelper(const char* pName);

  void PopulateInstanceLayerBypassDispatchTable();
  void PopulateDeviceLayerBypassDispatchTable();

 private:
  LayerBypassDispatch m_layerBypassDispatch = {};

  VkInstance m_instance = VK_NULL_HANDLE;
  VkDevice m_device = VK_NULL_HANDLE;

  std::mutex m_debug_utils_messenger_mutex;
  ska::flat_hash_map<VkDebugUtilsMessengerEXT,
                            VkDebugUtilsMessengerCreateInfoEXT>
      m_debug_utils_messengers;

  VkPhysicalDeviceMemoryProperties m_physDevMemProps = {};
  std::vector<bool> m_memTypeIsDeviceLocal;

  // TODO: Replace with my own data structure in the FUTURE
  ska::flat_hash_map<VkDeviceMemory, uint32_t> m_allocToMemTypeMap;
  ska::flat_hash_map<VkBuffer, uint32_t> m_bufferToMemTypeMap;

  bool m_index_buffer_is_device_local = false;
  bool m_vertex_buffers_are_device_local = false;
};

}  // namespace GWD

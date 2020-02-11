#pragma once

#include <vulkan/vulkan.h>

#include <unordered_map>

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

 protected:
  PFN_vkVoidFunction GetDeviceProcAddr_DispatchHelper(const char* pName);
  PFN_vkVoidFunction GetInstanceProcAddr_DispatchHelper(const char* pName);

  void PopulateInstanceLayerBypassDispatchTable();
  void PopulateDeviceLayerBypassDispatchTable();

 private:
  LayerBypassDispatch m_layerBypassDispatch = {};

  VkInstance m_instance = VK_NULL_HANDLE;
  VkDevice m_device = VK_NULL_HANDLE;

  VkPhysicalDeviceMemoryProperties m_physDevMemProps = {};

  // TODO: Replace with my own data structure in the FUTURE
  std::unordered_map<VkDeviceMemory, uint32_t> m_allocToMemTypeMap;
  std::unordered_map<VkBuffer, uint32_t> m_bufferToMemTypeMap;
};

}  // namespace GWD

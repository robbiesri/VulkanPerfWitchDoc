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

#define NOMINMAX

#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>
#include "vk_layer_dispatch_table.h"

#include <assert.h>
#include <mutex>
#include <unordered_map>

#include "WitchDoc.h"
#include "layerCore.h"

namespace GWDInterface {

// layer metadata

static constexpr const char* const kGwdVersion = "0.1";

static constexpr uint32_t kLoaderLayerInterfaceVersion = 2;

static constexpr const char* const kLayerName = "VK_LAYER_GOOGLE_witch_doctor";
static constexpr const char* const kLayerDescription =
    "Google's magical GPU performance layer for (mostly) Stadia";

static constexpr const uint32_t kLayerImplVersion = 1;
static constexpr const uint32_t kLayerSpecVersion = VK_API_VERSION_1_1;

// static const VkExtensionProperties s_deviceExtensions[] = {};
static const uint32_t s_numDeviceExtensions = 0;

// Dispatch tables required for routing instance and device calls onto the next
// layer in the dispatch chain among our handling of functions we intercept.
static ska::flat_hash_map<VkInstance, VkLayerInstanceDispatchTable>
    s_instance_dt;
static ska::flat_hash_map<VkDevice, VkLayerDispatchTable> s_device_dt;

// For finding a dispatch table in EnumeratePhysicalDeviceExtensionProperties
static ska::flat_hash_map<VkPhysicalDevice, VkInstance> s_device_instance_map;

// helper maps to find parent device until we try dispatch_keys
static ska::flat_hash_map<VkCommandBuffer, VkDevice> s_cmdbuf_device_map;
static ska::flat_hash_map<VkQueue, VkDevice> s_queue_device_map;

// Must protect access to state (maps above) by mutex since the Vulkan
// application may be calling these functions from different threads.
static std::mutex s_layer_mutex;
using LocalGuard = std::lock_guard<std::mutex>;

// TODO: Build some structure where we can check if certain downstream
// extensions actually exist, and if they don't, we don't have to check if we
// should dispatch
static uint32_t s_numDevices = 0;
static VkLayerDispatchTable* s_global_dispatch_table = nullptr;

typedef void* dispatch_key;
static inline dispatch_key get_dispatch_key(const void* object) {
  return (dispatch_key) * (VkLayerDispatchTable**)object;
}

static GWD::WitchDoctor WitchDoc_inst;

// Layer helper for external clients to dispatch
PFN_vkVoidFunction GwdGetDispatchedDeviceProcAddr(VkDevice device,
                                                  const char* pName) {
  LocalGuard lock(s_layer_mutex);
  return s_device_dt[device].GetDeviceProcAddr(device, pName);
}

PFN_vkVoidFunction GwdGetDispatchedInstanceProcAddr(VkInstance instance,
                                                    const char* pName) {
  LocalGuard lock(s_layer_mutex);
  return s_instance_dt[instance].GetInstanceProcAddr(instance, pName);
}

// ----------------------------------------------------------------------------
// Core layer logic
// ----------------------------------------------------------------------------

// TODO: For VkCommandBuffer and VkQueue, we store a map between VkCommandBuffer
// and VkQueue to VkDevice. We might want to see if we can use the same
// mechanism as the dispatch_key from layer_factory

VKAPI_ATTR VkResult VKAPI_CALL GwdCreateDebugUtilsMessengerEXT(
    VkInstance instance, VkDebugUtilsMessengerCreateInfoEXT const* pCreateInfo,
    VkAllocationCallbacks* pAllocator, VkDebugUtilsMessengerEXT* pMessenger) {
  PFN_vkCreateDebugUtilsMessengerEXT fp_CreateDebugUtilsMessengerEXT = nullptr;
  fp_CreateDebugUtilsMessengerEXT =
      s_instance_dt[instance].CreateDebugUtilsMessengerEXT;

  VkResult result = fp_CreateDebugUtilsMessengerEXT(instance, pCreateInfo,
                                                    pAllocator, pMessenger);

  result = WitchDoc_inst.PostCallCreateDebugUtilsMessengerEXT(
      result, instance, pCreateInfo, pAllocator, pMessenger);

  return result;
}

VKAPI_ATTR void VKAPI_CALL GwdDestroyDebugUtilsMessengerEXT(
    VkInstance instance, VkDebugUtilsMessengerEXT messenger,
    VkAllocationCallbacks* pAllocator) {
  PFN_vkDestroyDebugUtilsMessengerEXT fp_DestroyDebugUtilsMessengerEXT =
      nullptr;
  fp_DestroyDebugUtilsMessengerEXT =
      s_instance_dt[instance].DestroyDebugUtilsMessengerEXT;

  fp_DestroyDebugUtilsMessengerEXT(instance, messenger, pAllocator);

  WitchDoc_inst.PostCallDestroyDebugUtilsMessengerEXT(instance, messenger,
                                                      pAllocator);
}

VKAPI_ATTR void VKAPI_CALL GwdGetDeviceQueue(VkDevice device,
                                             uint32_t queueFamilyIndex,
                                             uint32_t queueIndex,
                                             VkQueue* pQueue) {
  PFN_vkGetDeviceQueue fp_GetDeviceQueue = nullptr;
  fp_GetDeviceQueue = s_global_dispatch_table->GetDeviceQueue;

  fp_GetDeviceQueue(device, queueFamilyIndex, queueIndex, pQueue);

  {
    LocalGuard lock(s_layer_mutex);
    s_queue_device_map[*pQueue] = device;
  }
}

VKAPI_ATTR VkResult VKAPI_CALL GwdQueueSubmit(VkQueue queue,
                                              uint32_t submitCount,
                                              const VkSubmitInfo* pSubmits,
                                              VkFence fence) {
  VkDevice device = s_queue_device_map[queue];

  PFN_vkQueueSubmit fp_QueueSubmit = nullptr;
  fp_QueueSubmit = s_global_dispatch_table->QueueSubmit;

  VkResult result = fp_QueueSubmit(queue, submitCount, pSubmits, fence);

  // TODO: Multiple VkSubmitInfo vs multiple VkCommandBuffer
  // WitchDoc_inst.PostCallQueueSubmit(queue, submitCount, pSubmits, fence);

  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL GwdAllocateMemory(
    VkDevice device, const VkMemoryAllocateInfo* pAllocateInfo,
    const VkAllocationCallbacks* pAllocator, VkDeviceMemory* pMemory) {
  PFN_vkAllocateMemory fp_AllocateMemory = nullptr;
  fp_AllocateMemory = s_global_dispatch_table->AllocateMemory;

  VkResult result =
      fp_AllocateMemory(device, pAllocateInfo, pAllocator, pMemory);

  result = WitchDoc_inst.PostCallAllocateMemory(result, device, pAllocateInfo,
                                                pAllocator, pMemory);

  return result;
}

VKAPI_ATTR void VKAPI_CALL
GwdFreeMemory(VkDevice device, VkDeviceMemory memory,
              const VkAllocationCallbacks* pAllocator) {
  PFN_vkFreeMemory fp_FreeMemory = nullptr;
  fp_FreeMemory = s_global_dispatch_table->FreeMemory;

  fp_FreeMemory(device, memory, pAllocator);

  WitchDoc_inst.PostCallFreeMemory(device, memory, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL GwdBindBufferMemory(VkDevice device,
                                                   VkBuffer buffer,
                                                   VkDeviceMemory memory,
                                                   VkDeviceSize memoryOffset) {
  PFN_vkBindBufferMemory fp_BindBufferMemory = nullptr;
  fp_BindBufferMemory = s_global_dispatch_table->BindBufferMemory;

  VkResult result = fp_BindBufferMemory(device, buffer, memory, memoryOffset);

  result = WitchDoc_inst.PostCallBindBufferMemory(result, device, buffer,
                                                  memory, memoryOffset);

  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL
GwdCreateBuffer(VkDevice device, const VkBufferCreateInfo* pCreateInfo,
                const VkAllocationCallbacks* pAllocator, VkBuffer* pBuffer) {
  PFN_vkCreateBuffer fp_CreateBuffer = nullptr;
  fp_CreateBuffer = s_global_dispatch_table->CreateBuffer;

  VkResult result = fp_CreateBuffer(device, pCreateInfo, pAllocator, pBuffer);

  result = WitchDoc_inst.PostCallCreateBuffer(result, device, pCreateInfo,
                                              pAllocator, pBuffer);

  return result;
}

VKAPI_ATTR void VKAPI_CALL GwdDestroyBuffer(
    VkDevice device, VkBuffer buffer, const VkAllocationCallbacks* pAllocator) {
  PFN_vkDestroyBuffer fp_DestroyBuffer = nullptr;
  fp_DestroyBuffer = s_global_dispatch_table->DestroyBuffer;

  fp_DestroyBuffer(device, buffer, pAllocator);

  WitchDoc_inst.PostCallDestroyBuffer(device, buffer, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL
GwdBindBufferMemory2(VkDevice device, uint32_t bindInfoCount,
                     const VkBindBufferMemoryInfo* pBindInfos) {
  PFN_vkBindBufferMemory2 fp_BindBufferMemory2 = nullptr;
  fp_BindBufferMemory2 = s_global_dispatch_table->BindBufferMemory2;

  VkResult result = fp_BindBufferMemory2(device, bindInfoCount, pBindInfos);

  result = WitchDoc_inst.PostCallBindBufferMemory2(result, device,
                                                   bindInfoCount, pBindInfos);

  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL GwdAllocateCommandBuffers(
    VkDevice device, const VkCommandBufferAllocateInfo* pAllocateInfo,
    VkCommandBuffer* pCommandBuffers) {
  PFN_vkAllocateCommandBuffers fp_AllocateCommandBuffers = nullptr;
  fp_AllocateCommandBuffers = s_global_dispatch_table->AllocateCommandBuffers;

  VkResult result =
      fp_AllocateCommandBuffers(device, pAllocateInfo, pCommandBuffers);

  if (VK_SUCCESS == result) {
    LocalGuard lock(s_layer_mutex);
    for (uint32_t cbIdx = 0; cbIdx < pAllocateInfo->commandBufferCount;
         cbIdx++) {
      s_cmdbuf_device_map[pCommandBuffers[cbIdx]] = device;
    }
  }

  return result;
}

VKAPI_ATTR void VKAPI_CALL GwdFreeCommandBuffers(
    VkDevice device, VkCommandPool commandPool, uint32_t commandBufferCount,
    const VkCommandBuffer* pCommandBuffers) {
  PFN_vkFreeCommandBuffers fp_FreeCommandBuffers = nullptr;
  fp_FreeCommandBuffers = s_global_dispatch_table->FreeCommandBuffers;

  fp_FreeCommandBuffers(device, commandPool, commandBufferCount,
                        pCommandBuffers);

  {
    LocalGuard lock(s_layer_mutex);
    for (uint32_t cbIdx = 0; cbIdx < commandBufferCount; cbIdx++) {
      s_cmdbuf_device_map.erase(pCommandBuffers[cbIdx]);
    }
  }
}

VKAPI_ATTR void VKAPI_CALL GwdCmdBindIndexBuffer(VkCommandBuffer commandBuffer,
                                                 VkBuffer buffer,
                                                 VkDeviceSize offset,
                                                 VkIndexType indexType) {
  // const VkDevice device = s_cmdbuf_device_map[commandBuffer];

  PFN_vkCmdBindIndexBuffer fp_CmdBindIndexBuffer = nullptr;
  fp_CmdBindIndexBuffer = s_global_dispatch_table->CmdBindIndexBuffer;

  fp_CmdBindIndexBuffer(commandBuffer, buffer, offset, indexType);

  WitchDoc_inst.PostCallCmdBindIndexBuffer(commandBuffer, buffer, offset,
                                           indexType);
}

VKAPI_ATTR void VKAPI_CALL GwdCmdBindVertexBuffers(
    VkCommandBuffer commandBuffer, uint32_t firstBinding, uint32_t bindingCount,
    const VkBuffer* pBuffers, const VkDeviceSize* pOffsets) {
  PFN_vkCmdBindVertexBuffers fp_CmdBindVertexBuffers = nullptr;
  fp_CmdBindVertexBuffers = s_global_dispatch_table->CmdBindVertexBuffers;

  fp_CmdBindVertexBuffers(commandBuffer, firstBinding, bindingCount, pBuffers,
                          pOffsets);

  WitchDoc_inst.PostCallCmdBindVertexBuffers(commandBuffer, firstBinding,
                                             bindingCount, pBuffers, pOffsets);
}

VKAPI_ATTR void VKAPI_CALL GwdCmdDraw(VkCommandBuffer commandBuffer,
                                      uint32_t vertexCount,
                                      uint32_t instanceCount,
                                      uint32_t firstVertex,
                                      uint32_t firstInstance) {
  PFN_vkCmdDraw fp_CmdDraw = nullptr;
  fp_CmdDraw = s_global_dispatch_table->CmdDraw;

  fp_CmdDraw(commandBuffer, vertexCount, instanceCount, firstVertex,
             firstInstance);

  WitchDoc_inst.PostCallCmdDraw(commandBuffer, vertexCount, instanceCount,
                                firstVertex, firstInstance);
}

VKAPI_ATTR void VKAPI_CALL GwdCmdDrawIndexed(
    VkCommandBuffer commandBuffer, uint32_t indexCount, uint32_t instanceCount,
    uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) {
  PFN_vkCmdDrawIndexed fp_CmdDrawIndexed = nullptr;
  fp_CmdDrawIndexed = s_global_dispatch_table->CmdDrawIndexed;

  fp_CmdDrawIndexed(commandBuffer, indexCount, instanceCount, firstIndex,
                    vertexOffset, firstInstance);

  WitchDoc_inst.PostCallCmdDrawIndexed(commandBuffer, indexCount, instanceCount,
                                       firstIndex, vertexOffset, firstInstance);
}

VKAPI_ATTR void VKAPI_CALL GwdCmdDrawIndirect(VkCommandBuffer commandBuffer,
                                              VkBuffer buffer,
                                              VkDeviceSize offset,
                                              uint32_t drawCount,
                                              uint32_t stride) {
  PFN_vkCmdDrawIndirect fp_CmdDrawIndirect = nullptr;
  fp_CmdDrawIndirect = s_global_dispatch_table->CmdDrawIndirect;

  fp_CmdDrawIndirect(commandBuffer, buffer, offset, drawCount, stride);

  WitchDoc_inst.PostCallCmdDrawIndirect(commandBuffer, buffer, offset,
                                        drawCount, stride);
}

VKAPI_ATTR void VKAPI_CALL GwdCmdDrawIndexedIndirect(
    VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset,
    uint32_t drawCount, uint32_t stride) {
  PFN_vkCmdDrawIndexedIndirect fp_CmdDrawIndexedIndirect = nullptr;
  fp_CmdDrawIndexedIndirect = s_global_dispatch_table->CmdDrawIndexedIndirect;

  fp_CmdDrawIndexedIndirect(commandBuffer, buffer, offset, drawCount, stride);

  WitchDoc_inst.PostCallCmdDrawIndexedIndirect(commandBuffer, buffer, offset,
                                               drawCount, stride);
}

// ----------------------------------------------------------------------------
// Layer glue code
// ----------------------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL GwdEnumerateInstanceLayerProperties(
    uint32_t* pPropertyCount, VkLayerProperties* pProperties) {
  // Vulkan spec dictates that we are only supposed to enumerate ourself
  if (pPropertyCount) *pPropertyCount = 1;
  if (pProperties) {
    strcpy(pProperties->layerName, kLayerName);
    strcpy(pProperties->description, kLayerDescription);
    pProperties->implementationVersion = kLayerImplVersion;
    pProperties->specVersion = kLayerSpecVersion;
  }

  return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL GwdEnumerateInstanceExtensionProperties(
    const char* pLayerName, uint32_t* pPropertyCount,
    VkExtensionProperties* pProperties) {
  // Inform the client that we have no extension properties if this layer
  // specifically is being queried.
  if (pLayerName != nullptr && strcmp(pLayerName, kLayerName) == 0) {
    if (pPropertyCount) *pPropertyCount = 0;
    return VK_SUCCESS;
  }

  // Vulkan spec mandates returning this when this layer isn't being queried.
  return VK_ERROR_LAYER_NOT_PRESENT;
}

VKAPI_ATTR VkResult VKAPI_CALL
GwdEnumeratePhysicalDevices(VkInstance instance, uint32_t* pPhysicalDeviceCount,
                            VkPhysicalDevice* pPhysicalDevices) {
  PFN_vkEnumeratePhysicalDevices fp_EnumeratePhysicalDevices = nullptr;
  {
    LocalGuard lock(s_layer_mutex);
    fp_EnumeratePhysicalDevices =
        s_instance_dt[instance].EnumeratePhysicalDevices;
  }
  VkResult result = fp_EnumeratePhysicalDevices(instance, pPhysicalDeviceCount,
                                                pPhysicalDevices);

  if (pPhysicalDeviceCount != nullptr && pPhysicalDevices != nullptr) {
    // Map these devices to this instance so that we can map each physical
    // device back to a dispatch table when handling
    // EnumerateDeviceExtensionProperties. Note that this is hardly error-proof,
    // but it's impossible to handle this perfectly since physical devices may
    // be in use by multiple instances.
    {
      LocalGuard lock(s_layer_mutex);
      for (uint32_t i = 0; i < *pPhysicalDeviceCount; ++i) {
        s_device_instance_map[pPhysicalDevices[i]] = instance;
      }
    }
  }

  return result;
}

// Deprecated by Khronos, but we'll support it in case older applications still
// use it.
VKAPI_ATTR VkResult VKAPI_CALL GwdEnumerateDeviceLayerProperties(
    VkPhysicalDevice physicalDevice, uint32_t* pPropertyCount,
    VkLayerProperties* pProperties) {
  // This function is supposed to return the same results as
  // EnumerateInstanceLayerProperties since device layers were deprecated.
  return GwdEnumerateInstanceLayerProperties(pPropertyCount, pProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL GwdEnumerateDeviceExtensionProperties(
    VkPhysicalDevice physicalDevice, const char* pLayerName,
    uint32_t* pPropertyCount, VkExtensionProperties* pProperties) {
  // If only our layer is being queried, just return our listing
  if (pLayerName != nullptr && strcmp(pLayerName, kLayerName) == 0) {
    VkResult layerOnlyResult = VK_SUCCESS;

    if (nullptr == pProperties) {
      *pPropertyCount = s_numDeviceExtensions;
    } else {
      auto numExtensionsToCopy = s_numDeviceExtensions;
      if (*pPropertyCount < s_numDeviceExtensions) {
        numExtensionsToCopy = *pPropertyCount;
      }

      if (numExtensionsToCopy > 0) {
        // memcpy(pProperties, s_deviceExtensions,
        //       numExtensionsToCopy * sizeof(VkExtensionProperties));
      }
      *pPropertyCount = numExtensionsToCopy;

      if (numExtensionsToCopy < s_numDeviceExtensions) {
        layerOnlyResult = VK_INCOMPLETE;
      }
    }

    return layerOnlyResult;
  }

  // if this is a general query, we have to get the extensions from down
  // the callchain, and pass them up with our extension appended

  PFN_vkEnumerateDeviceExtensionProperties
      fp_EnumerateDeviceExtensionProperties = nullptr;
  {
    LocalGuard lock(s_layer_mutex);
    VkInstance instance = s_device_instance_map[physicalDevice];
    fp_EnumerateDeviceExtensionProperties =
        s_instance_dt[instance].EnumerateDeviceExtensionProperties;
  }

  if (pLayerName != nullptr) {
    // if this is another layer, we can just return the data from it!
    return fp_EnumerateDeviceExtensionProperties(physicalDevice, pLayerName,
                                                 pPropertyCount, pProperties);
  }

  uint32_t numOtherExtensions = 0;
  VkResult result = fp_EnumerateDeviceExtensionProperties(
      physicalDevice, nullptr, &numOtherExtensions, nullptr);
  if (result != VK_SUCCESS) {
    return result;
  }

  std::vector<VkExtensionProperties> extensions(numOtherExtensions);
  result = fp_EnumerateDeviceExtensionProperties(
      physicalDevice, nullptr, &numOtherExtensions, &extensions[0]);
  if (result != VK_SUCCESS) {
    return result;
  }

  // let's scan the list of extensions from down the chain, and add our unique
  // extensions
  // for (uint32_t extIdx = 0; extIdx < s_numDeviceExtensions; extIdx++) {
  //  auto curDeviceExt = s_deviceExtensions[extIdx];
  //  bool uniqueExtension = true;

  //  for (auto extProps : extensions) {
  //    if (0 == strcmp(extProps.extensionName, curDeviceExt.extensionName)) {
  //      uniqueExtension = false;
  //      break;
  //    }
  //  }

  //  if (uniqueExtension) {
  //    extensions.push_back(curDeviceExt);
  //  }
  //}

  if (nullptr == pProperties) {
    // just a count
    *pPropertyCount = uint32_t(extensions.size());
  } else {
    uint32_t numExtToCopy = uint32_t(extensions.size());
    if (numExtToCopy > *pPropertyCount) {
      numExtToCopy = *pPropertyCount;
    }

    for (uint32_t copyIdx = 0; copyIdx < numExtToCopy; copyIdx++) {
      pProperties[copyIdx] = extensions[copyIdx];
    }

    *pPropertyCount = numExtToCopy;

    if (numExtToCopy < extensions.size()) {
      return VK_INCOMPLETE;
    }
  }

  return VK_SUCCESS;
}

// ----------------------------------------------------------------------------
// Layer bootstrapping code
// ----------------------------------------------------------------------------

#define GWD_GETINSTDISPATCHADDR(func) \
  dispatch_table.func = (PFN_vk##func)next_gipa(*pInstance, "vk" #func);

VKAPI_ATTR VkResult VKAPI_CALL GwdCreateInstance(
    const VkInstanceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator, VkInstance* pInstance) {
  VkLayerInstanceCreateInfo* layer_ci =
      (VkLayerInstanceCreateInfo*)pCreateInfo->pNext;

  // Search through linked structs in pNext for link info.
  while (layer_ci &&
         (layer_ci->sType != VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO ||
          layer_ci->function != VK_LAYER_LINK_INFO)) {
    layer_ci = (VkLayerInstanceCreateInfo*)layer_ci->pNext;
  }

  if (layer_ci == nullptr) {
    // No link info was found; we can't finish initializing
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  PFN_vkGetInstanceProcAddr next_gipa =
      layer_ci->u.pLayerInfo->pfnNextGetInstanceProcAddr;

  // Advance linkage for next layer
  layer_ci->u.pLayerInfo = layer_ci->u.pLayerInfo->pNext;

  // Need to call vkCreateInstance down the chain to actually create the
  // instance.
  PFN_vkCreateInstance create_instance =
      (PFN_vkCreateInstance)next_gipa(VK_NULL_HANDLE, "vkCreateInstance");
  VkResult result = create_instance(pCreateInfo, pAllocator, pInstance);

  VkLayerInstanceDispatchTable dispatch_table = {};
  GWD_GETINSTDISPATCHADDR(GetInstanceProcAddr);
  GWD_GETINSTDISPATCHADDR(DestroyInstance);
  GWD_GETINSTDISPATCHADDR(CreateDebugUtilsMessengerEXT);
  GWD_GETINSTDISPATCHADDR(DestroyDebugUtilsMessengerEXT);
  GWD_GETINSTDISPATCHADDR(EnumerateDeviceExtensionProperties);
  GWD_GETINSTDISPATCHADDR(EnumeratePhysicalDevices);

  {
    LocalGuard lock(s_layer_mutex);
    s_instance_dt[*pInstance] = dispatch_table;
  }

  WitchDoc_inst.PostCallCreateInstance(pCreateInfo, pAllocator, pInstance);

  return result;
}

#undef GWD_GETINSTDISPATCHADDR

VKAPI_ATTR void VKAPI_CALL GwdDestroyInstance(
    VkInstance instance, const VkAllocationCallbacks* pAllocator) {
  LocalGuard lock(s_layer_mutex);
  s_instance_dt.erase(instance);

  // TODO: Call down the chain??
}

#define GWD_GETDEVDISPATCHADDR(func) \
  dispatch_table.func = (PFN_vk##func)next_gdpa(*pDevice, "vk" #func);

VKAPI_ATTR VkResult VKAPI_CALL GwdCreateDevice(
    VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator, VkDevice* pDevice) {
  VkLayerDeviceCreateInfo* layer_ci =
      (VkLayerDeviceCreateInfo*)pCreateInfo->pNext;

  // Search through linked structs in pNext for link info.
  while (layer_ci &&
         (layer_ci->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO ||
          layer_ci->function != VK_LAYER_LINK_INFO)) {
    layer_ci = (VkLayerDeviceCreateInfo*)layer_ci->pNext;
  }

  if (layer_ci == nullptr) {
    // No link info was found; we can't finish initializing
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  PFN_vkGetInstanceProcAddr next_gipa =
      layer_ci->u.pLayerInfo->pfnNextGetInstanceProcAddr;
  PFN_vkGetDeviceProcAddr next_gdpa =
      layer_ci->u.pLayerInfo->pfnNextGetDeviceProcAddr;

  // Advance linkage for next layer
  layer_ci->u.pLayerInfo = layer_ci->u.pLayerInfo->pNext;

  // Need to call vkCreateDevice down the chain to actually create the device
  PFN_vkCreateDevice createFunc =
      (PFN_vkCreateDevice)next_gipa(VK_NULL_HANDLE, "vkCreateDevice");
  VkResult result =
      createFunc(physicalDevice, pCreateInfo, pAllocator, pDevice);

  // TODO: Instead of the manual fetch of proc addresses, we might want to use
  // the functionality in LayerFactor/vk_dispatch_table_helper.h We can fill in
  // the dispatch_tables, and leverage it for both the forwarding logic in the
  // layer intercept points, and the layer's own usage of Vulkan APIs in order
  // to manage/use Vulkan stuff This would modify the behavior of
  // GwdCreateInstance, GwdCreateDevice, GwdGetDispatchedInstanceProcAddr and
  // GwdGetDispatchedDeviceProcAddr.

  VkLayerDispatchTable dispatch_table = {};
  GWD_GETDEVDISPATCHADDR(GetDeviceProcAddr);
  GWD_GETDEVDISPATCHADDR(DestroyDevice);
  GWD_GETDEVDISPATCHADDR(GetDeviceQueue);
  GWD_GETDEVDISPATCHADDR(QueueSubmit);
  GWD_GETDEVDISPATCHADDR(AllocateMemory);
  GWD_GETDEVDISPATCHADDR(FreeMemory);
  GWD_GETDEVDISPATCHADDR(BindBufferMemory);
  GWD_GETDEVDISPATCHADDR(CreateBuffer);
  GWD_GETDEVDISPATCHADDR(DestroyBuffer);
  GWD_GETDEVDISPATCHADDR(BindBufferMemory2);
  GWD_GETDEVDISPATCHADDR(CmdDraw);
  GWD_GETDEVDISPATCHADDR(CmdDrawIndexed);
  GWD_GETDEVDISPATCHADDR(CmdDrawIndirect);
  GWD_GETDEVDISPATCHADDR(CmdDrawIndexedIndirect);
  GWD_GETDEVDISPATCHADDR(AllocateCommandBuffers);
  GWD_GETDEVDISPATCHADDR(FreeCommandBuffers);
  GWD_GETDEVDISPATCHADDR(CmdBindIndexBuffer);
  GWD_GETDEVDISPATCHADDR(CmdBindVertexBuffers);

  {
    LocalGuard lock(s_layer_mutex);
    s_device_dt[*pDevice] = dispatch_table;
    s_numDevices++;
    if (1 == s_numDevices) {
      s_global_dispatch_table = &s_device_dt[*pDevice];
    } else if (s_numDevices > 1) {
      assert(!"Too many devices for WitchDoctor");
    }
  }

  WitchDoc_inst.PostCallCreateDevice(physicalDevice, pCreateInfo, pAllocator,
                                     pDevice);

  return result;
}

#undef GWD_GETDEVDISPATCHADDR

VKAPI_ATTR void VKAPI_CALL
GwdDestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator) {
  LocalGuard lock(s_layer_mutex);
  s_device_dt.erase(device);
  s_numDevices--;
}

#define GWD_GETPROCADDR(func) \
  if (strcmp(pName, "vk" #func) == 0) return (PFN_vkVoidFunction)&Gwd##func;

// GetDeviceProcAddr is declared before GetInstanceProcAddr because otherwise
// we'd need a forward declaration of GwdGetDeviceProcAddr -_-
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
GwdGetDeviceProcAddr(VkDevice device, const char* pName) {
  // Functions available through GetInstanceProcAddr and GetDeviceProcAddr
  GWD_GETPROCADDR(GetDeviceProcAddr);
  GWD_GETPROCADDR(EnumerateDeviceLayerProperties);
  GWD_GETPROCADDR(EnumerateDeviceExtensionProperties);
  GWD_GETPROCADDR(CreateDevice);
  GWD_GETPROCADDR(DestroyDevice);
  GWD_GETPROCADDR(GetDeviceQueue);
  GWD_GETPROCADDR(QueueSubmit);
  GWD_GETPROCADDR(AllocateMemory);
  GWD_GETPROCADDR(FreeMemory);
  GWD_GETPROCADDR(BindBufferMemory);
  GWD_GETPROCADDR(CreateBuffer);
  GWD_GETPROCADDR(DestroyBuffer);
  GWD_GETPROCADDR(BindBufferMemory2);
  GWD_GETPROCADDR(CmdDraw);
  GWD_GETPROCADDR(CmdDrawIndexed);
  GWD_GETPROCADDR(CmdDrawIndirect);
  GWD_GETPROCADDR(CmdDrawIndexedIndirect);
  GWD_GETPROCADDR(AllocateCommandBuffers);
  GWD_GETPROCADDR(FreeCommandBuffers);
  GWD_GETPROCADDR(CmdBindIndexBuffer);
  GWD_GETPROCADDR(CmdBindVertexBuffers);

  {
    LocalGuard lock(s_layer_mutex);
    return s_device_dt[device].GetDeviceProcAddr(device, pName);
  }
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
GwdGetInstanceProcAddr(VkInstance instance, const char* pName) {
  // Functions available only through GetInstanceProcAddr
  GWD_GETPROCADDR(GetInstanceProcAddr);
  GWD_GETPROCADDR(CreateInstance);
  GWD_GETPROCADDR(DestroyInstance);
  GWD_GETPROCADDR(CreateDebugUtilsMessengerEXT);
  GWD_GETPROCADDR(DestroyDebugUtilsMessengerEXT);
  GWD_GETPROCADDR(EnumerateInstanceLayerProperties);
  GWD_GETPROCADDR(EnumerateInstanceExtensionProperties);
  GWD_GETPROCADDR(EnumeratePhysicalDevices);

  // Functions available through GetInstanceProcAddr and GetDeviceProcAddr
  GWD_GETPROCADDR(GetDeviceProcAddr);
  GWD_GETPROCADDR(EnumerateDeviceLayerProperties);
  GWD_GETPROCADDR(EnumerateDeviceExtensionProperties);
  GWD_GETPROCADDR(CreateDevice);
  GWD_GETPROCADDR(DestroyDevice);
  GWD_GETPROCADDR(GetDeviceQueue);
  GWD_GETPROCADDR(QueueSubmit);
  GWD_GETPROCADDR(AllocateMemory);
  GWD_GETPROCADDR(FreeMemory);
  GWD_GETPROCADDR(BindBufferMemory);
  GWD_GETPROCADDR(CreateBuffer);
  GWD_GETPROCADDR(DestroyBuffer);
  GWD_GETPROCADDR(BindBufferMemory2);
  GWD_GETPROCADDR(CmdDraw);
  GWD_GETPROCADDR(CmdDrawIndexed);
  GWD_GETPROCADDR(CmdDrawIndirect);
  GWD_GETPROCADDR(CmdDrawIndexedIndirect);
  GWD_GETPROCADDR(AllocateCommandBuffers);
  GWD_GETPROCADDR(FreeCommandBuffers);
  GWD_GETPROCADDR(CmdBindIndexBuffer);
  GWD_GETPROCADDR(CmdBindVertexBuffers);

  {
    LocalGuard lock(s_layer_mutex);
    return s_instance_dt[instance].GetInstanceProcAddr(instance, pName);
  }
}

#undef GWD_GETPROCADDR

// TODO: Not clear if we really need the __declspec for Windows
// The linker complains about functions being exported multiple times
// but if we don't use declspec, the functions aren't latched by the loader.
#if defined(WIN32)
#define WITCH_DOCTOR_EXPORT extern "C" __declspec(dllexport) VK_LAYER_EXPORT
#else
#define WITCH_DOCTOR_EXPORT extern "C" VK_LAYER_EXPORT
#endif

// This is only needed on pre-1.1.82 Vulkan loaders
// For whatever reason, the loader looks for vkCreateDevice from the exported
// GetInstanceProcAddr instead of what is specified in
// NegotiateLoaderLayerInterfaceVersion.
//#define LOADER_PROC_HACK

#if defined(LOADER_PROC_HACK)
WITCH_DOCTOR_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice dev, const char* funcName) {
  return GWDInterface::GwdGetDeviceProcAddr(dev, funcName);
}

WITCH_DOCTOR_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char* funcName) {
  return GWDInterface::GwdGetInstanceProcAddr(instance, funcName);
}
#endif

// layer export
WITCH_DOCTOR_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
gwdNegotiateLoaderLayerInterfaceVersion(
    VkNegotiateLayerInterface* pVersionStruct) {
  if (pVersionStruct == NULL ||
      pVersionStruct->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  // We don't support older interface versions.
  if (pVersionStruct->loaderLayerInterfaceVersion <
      kLoaderLayerInterfaceVersion) {
    return VK_ERROR_INITIALIZATION_FAILED;
  } else {
    pVersionStruct->loaderLayerInterfaceVersion = kLoaderLayerInterfaceVersion;
  }

  pVersionStruct->pfnGetInstanceProcAddr = &GwdGetInstanceProcAddr;
  pVersionStruct->pfnGetDeviceProcAddr = &GwdGetDeviceProcAddr;
  // This is null because we have no physical device extensions
  pVersionStruct->pfnGetPhysicalDeviceProcAddr = nullptr;

  return VK_SUCCESS;
}

#undef WITCH_DOCTOR_EXPORT

}  // namespace GWDInterface

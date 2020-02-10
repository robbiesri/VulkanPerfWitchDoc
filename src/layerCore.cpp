#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include "vk_layer_dispatch_table.h"

#include <mutex>
#include <unordered_map>

//#include "GPUVoyeur.h"
#include "layerCore.h"

namespace GV {

// layer metadata

static constexpr const char* const kGvVersion = "0.1";

static constexpr uint32_t kLoaderLayerInterfaceVersion = 2;

static constexpr const char* const kLayerName =
    "VK_LAYER_PERFHAUS_GPUVoyeur";
static constexpr const char* const kLayerDescription =
    "Google's coarse profiling layer for da GPU";

static constexpr const uint32_t kLayerImplVersion = 1;
static constexpr const uint32_t kLayerSpecVersion = VK_API_VERSION_1_1;

// TODO: Add support for VK_EXT_debug_utils (BLEH)
static const VkExtensionProperties s_deviceExtensions[1] = { { VK_EXT_DEBUG_MARKER_EXTENSION_NAME, VK_EXT_DEBUG_MARKER_SPEC_VERSION } };
static const uint32_t s_numDeviceExtensions = uint32_t(sizeof(s_deviceExtensions) / sizeof(VkExtensionProperties));

// Dispatch tables required for routing instance and device calls onto the next
// layer in the dispatch chain among our handling of functions we intercept.
static std::unordered_map<VkInstance, VkLayerInstanceDispatchTable> s_instance_dt;
static std::unordered_map<VkDevice, VkLayerDispatchTable> s_device_dt;

// For finding a dispatch table in EnumeratePhysicalDeviceExtensionProperties
static std::unordered_map<VkPhysicalDevice, VkInstance> s_device_instance_map;

// helper maps to find parent device until we try dispatch_keys
static std::unordered_map<VkCommandBuffer, VkDevice> s_cmdbuf_device_map;
static std::unordered_map<VkQueue, VkDevice> s_queue_device_map;

// Must protect access to state (maps above) by mutex since the Vulkan
// application may be calling these functions from different threads.
static std::mutex s_layer_mutex;
using LocalGuard = std::lock_guard<std::mutex>;

// TODO: Build some structure where we can check if certain downstream extensions
// actually exist, and if they don't, we don't have to check if we should dispatch
static uint32_t s_numDevices = 0;
static VkLayerDispatchTable * s_global_dispatch_table = nullptr;
static bool s_downstreamDebugMarker = false;

typedef void *dispatch_key;
static inline dispatch_key get_dispatch_key(const void *object) { return (dispatch_key) * (VkLayerDispatchTable **)object; }

//static GPUVoyeur GPUVoyeur_inst;

// Layer helper for external clients to dispatch
PFN_vkVoidFunction GvGetDispatchedDeviceProcAddr(VkDevice device, const char* pName)
{
    LocalGuard lock(s_layer_mutex);
    return s_device_dt[device].GetDeviceProcAddr(device, pName);
}

PFN_vkVoidFunction GvGetDispatchedInstanceProcAddr(VkInstance instance, const char* pName)
{
    LocalGuard lock(s_layer_mutex);
    return s_instance_dt[instance].GetInstanceProcAddr(instance, pName);
}

// ----------------------------------------------------------------------------
// Core layer logic
// ----------------------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL GvCreateCommandPool(
    VkDevice                                    device,
    const VkCommandPoolCreateInfo*              pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkCommandPool*                              pCommandPool)
{
    PFN_vkCreateCommandPool fp_CreateCommandPool = nullptr;
    fp_CreateCommandPool = s_device_dt[device].CreateCommandPool;

    VkResult result = fp_CreateCommandPool(device, pCreateInfo, pAllocator, pCommandPool);

    //GPUVoyeur_inst.PostCallCreateCommandPool(device, pCreateInfo, pAllocator, pCommandPool);

    return result;
}

VKAPI_ATTR void VKAPI_CALL GvDestroyCommandPool(
    VkDevice                                    device,
    VkCommandPool                               commandPool,
    const VkAllocationCallbacks*                pAllocator)
{
    PFN_vkDestroyCommandPool fp_DestroyCommandPool = nullptr;
    fp_DestroyCommandPool = s_device_dt[device].DestroyCommandPool;

    fp_DestroyCommandPool(device, commandPool, pAllocator);

    //GPUVoyeur_inst.PostCallDestroyCommandPool(device, commandPool, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL GvResetCommandPool(
    VkDevice                                    device,
    VkCommandPool                               commandPool,
    VkCommandPoolResetFlags                     flags)
{
    PFN_vkResetCommandPool fp_ResetCommandPool = nullptr;
    fp_ResetCommandPool = s_device_dt[device].ResetCommandPool;

    VkResult result = fp_ResetCommandPool(device, commandPool, flags);

    //GPUVoyeur_inst.PostCallResetCommandPool(device, commandPool, flags);

    return result;
}

// TODO: For VkCommandBuffer and VkQueue, we store a map between VkCommandBuffer and VkQueue to
// VkDevice. We might want to see if we can use the same mechanism as the dispatch_key from layer_factory

VKAPI_ATTR VkResult VKAPI_CALL GvAllocateCommandBuffers(
    VkDevice                                    device,
    const VkCommandBufferAllocateInfo*          pAllocateInfo,
    VkCommandBuffer*                            pCommandBuffers)
{
    PFN_vkAllocateCommandBuffers fp_AllocateCommandBuffers = nullptr;
    fp_AllocateCommandBuffers = s_device_dt[device].AllocateCommandBuffers;

    VkResult result = fp_AllocateCommandBuffers(device, pAllocateInfo, pCommandBuffers);

    if (VK_SUCCESS == result)
    {
        LocalGuard lock(s_layer_mutex);
        for (uint32_t cbIdx = 0; cbIdx < pAllocateInfo->commandBufferCount; cbIdx++)
        {
            s_cmdbuf_device_map[pCommandBuffers[cbIdx]] = device;
        }
    }

    //GPUVoyeur_inst.PostCallAllocateCommandBuffers(device, pAllocateInfo, pCommandBuffers);

    return result;
}

VKAPI_ATTR void VKAPI_CALL GvFreeCommandBuffers(
    VkDevice                                    device,
    VkCommandPool                               commandPool,
    uint32_t                                    commandBufferCount,
    const VkCommandBuffer*                      pCommandBuffers)
{
    PFN_vkFreeCommandBuffers fp_FreeCommandBuffers = nullptr;
    fp_FreeCommandBuffers = s_device_dt[device].FreeCommandBuffers;

    fp_FreeCommandBuffers(device, commandPool, commandBufferCount, pCommandBuffers);

    {
        LocalGuard lock(s_layer_mutex);
        for (uint32_t cbIdx = 0; cbIdx < commandBufferCount; cbIdx++)
        {
            s_cmdbuf_device_map.erase(pCommandBuffers[cbIdx]);
        }
    }

    //GPUVoyeur_inst.PostCallFreeCommandBuffers(device, commandPool, commandBufferCount, pCommandBuffers);
}

VKAPI_ATTR VkResult VKAPI_CALL GvBeginCommandBuffer(
    VkCommandBuffer                             commandBuffer,
    const VkCommandBufferBeginInfo*             pBeginInfo)
{
    VkDevice device = s_cmdbuf_device_map[commandBuffer];

    PFN_vkBeginCommandBuffer fp_BeginCommandBuffer = nullptr;
    fp_BeginCommandBuffer = s_device_dt[device].BeginCommandBuffer;

    VkResult result = fp_BeginCommandBuffer(commandBuffer, pBeginInfo);

    //GPUVoyeur_inst.PostCallBeginCommandBuffer(commandBuffer, pBeginInfo);

    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL GvEndCommandBuffer(
    VkCommandBuffer                             commandBuffer)
{
    VkDevice device = s_cmdbuf_device_map[commandBuffer];

    PFN_vkEndCommandBuffer fp_EndCommandBuffer = nullptr;
    fp_EndCommandBuffer = s_device_dt[device].EndCommandBuffer;

    //GPUVoyeur_inst.PreCallEndCommandBuffer(commandBuffer);

    VkResult result = fp_EndCommandBuffer(commandBuffer);

    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL GvResetCommandBuffer(
    VkCommandBuffer                             commandBuffer,
    VkCommandBufferResetFlags                   flags)
{
    VkDevice device = s_cmdbuf_device_map[commandBuffer];

    PFN_vkResetCommandBuffer fp_ResetCommandBuffer = nullptr;
    fp_ResetCommandBuffer = s_device_dt[device].ResetCommandBuffer;

    //GPUVoyeur_inst.PreCallResetCommandBuffer(commandBuffer, flags);

    VkResult result = fp_ResetCommandBuffer(commandBuffer, flags);

    return result;
}

VKAPI_ATTR void VKAPI_CALL GvGetDeviceQueue(
    VkDevice                                    device,
    uint32_t                                    queueFamilyIndex,
    uint32_t                                    queueIndex,
    VkQueue*                                    pQueue)
{
    PFN_vkGetDeviceQueue fp_GetDeviceQueue = nullptr;
    fp_GetDeviceQueue = s_device_dt[device].GetDeviceQueue;

    fp_GetDeviceQueue(device, queueFamilyIndex, queueIndex, pQueue);

    {
        LocalGuard lock(s_layer_mutex);
        s_queue_device_map[*pQueue] = device;
    }

    //GPUVoyeur_inst.PostCallGetDeviceQueue(device, queueFamilyIndex, queueIndex, pQueue);
}

VKAPI_ATTR VkResult VKAPI_CALL GvQueueSubmit(
    VkQueue                                     queue,
    uint32_t                                    submitCount,
    const VkSubmitInfo*                         pSubmits,
    VkFence                                     fence)
{
    VkDevice device = s_queue_device_map[queue];

    PFN_vkQueueSubmit fp_QueueSubmit = nullptr;
    fp_QueueSubmit = s_device_dt[device].QueueSubmit;

    //GPUVoyeur_inst.PreCallQueueSubmit(queue, submitCount, pSubmits, fence);

    VkResult result = fp_QueueSubmit(queue, submitCount, pSubmits, fence);

    //GPUVoyeur_inst.PostCallQueueSubmit(queue, submitCount, pSubmits, fence);

    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL GvQueuePresentKHR(
    VkQueue                                     queue,
    const VkPresentInfoKHR*                     pPresentInfo)
{
    VkDevice device = s_queue_device_map[queue];

    PFN_vkQueuePresentKHR fp_QueuePresentKHR = nullptr;
    fp_QueuePresentKHR = s_device_dt[device].QueuePresentKHR;

    VkResult result = fp_QueuePresentKHR(queue, pPresentInfo);

    //GPUVoyeur_inst.PostCallQueuePresentKHR(queue, pPresentInfo);

    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL GvAcquireNextImageKHR(
    VkDevice                                    device,
    VkSwapchainKHR                              swapchain,
    uint64_t                                    timeout,
    VkSemaphore                                 semaphore,
    VkFence                                     fence,
    uint32_t*                                   pImageIndex)
{
    PFN_vkAcquireNextImageKHR fp_AcquireNextImageKHR = nullptr;
    fp_AcquireNextImageKHR = s_device_dt[device].AcquireNextImageKHR;

    //GPUVoyeur_inst.PreCallAcquireNextImageKHR(device, swapchain, timeout, semaphore, fence, pImageIndex);

    VkResult result = fp_AcquireNextImageKHR(device, swapchain, timeout, semaphore, fence, pImageIndex);

    return result;
}

// VK_EXT_debug_marker

VKAPI_ATTR VkResult VKAPI_CALL GvDebugMarkerSetObjectTagEXT(
    VkDevice                                    device,
    const VkDebugMarkerObjectTagInfoEXT*        pTagInfo)
{
    VkResult result = VK_SUCCESS;

    if (s_device_dt[device].DebugMarkerSetObjectTagEXT != nullptr)
    {
        result = s_device_dt[device].DebugMarkerSetObjectTagEXT(device, pTagInfo);
    }

    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL GvDebugMarkerSetObjectNameEXT(
    VkDevice                                    device,
    const VkDebugMarkerObjectNameInfoEXT*       pNameInfo)
{
    VkResult result = VK_SUCCESS;
    
    if (s_device_dt[device].DebugMarkerSetObjectNameEXT != nullptr)
    {
        result = s_device_dt[device].DebugMarkerSetObjectNameEXT(device, pNameInfo);
    }

    return result;
}

VKAPI_ATTR void VKAPI_CALL GvCmdDebugMarkerBeginEXT(
    VkCommandBuffer                             commandBuffer,
    const VkDebugMarkerMarkerInfoEXT*           pMarkerInfo)
{
    // TODO: Do I check for VK_STRUCTURE_TYPE_DEBUG_MARKER_MARKER_INFO_EXT?

    // TODO: We hit a huge perf cliff because of the frequency that DebugMarker
    // APIs can get called. These map lookups are brutal. We would try to improve
    // the map lookups using the dispatch key. Alternatively, why are we supporting
    // multiple devices? Can we pretend only one device?
    // Though...the perf is ok in a release build...

    if (s_downstreamDebugMarker)
    {
        VkDevice device = s_cmdbuf_device_map[commandBuffer];

        if (s_device_dt[device].CmdDebugMarkerBeginEXT != nullptr)
        {
            s_device_dt[device].CmdDebugMarkerBeginEXT(commandBuffer, pMarkerInfo);
        }
    }

    //if (s_global_dispatch_table->CmdDebugMarkerBeginEXT != nullptr)
    //{
    //    s_global_dispatch_table->CmdDebugMarkerBeginEXT(commandBuffer, pMarkerInfo);
    //}

    //GPUVoyeur_inst.PostCallCmdDebugMarkerBeginEXT(commandBuffer, pMarkerInfo);
}

VKAPI_ATTR void VKAPI_CALL GvCmdDebugMarkerEndEXT(
    VkCommandBuffer                             commandBuffer)
{
    //GPUVoyeur_inst.PreCallCmdDebugMarkerEndEXT(commandBuffer);

    if (s_downstreamDebugMarker)
    {
        VkDevice device = s_cmdbuf_device_map[commandBuffer];

        if (s_device_dt[device].CmdDebugMarkerEndEXT != nullptr)
        {
            s_device_dt[device].CmdDebugMarkerEndEXT(commandBuffer);
        }
    }

    //if (s_global_dispatch_table->CmdDebugMarkerEndEXT != nullptr)
    //{
    //    s_global_dispatch_table->CmdDebugMarkerEndEXT(commandBuffer);
    //}
}

VKAPI_ATTR void VKAPI_CALL GvCmdDebugMarkerInsertEXT(
    VkCommandBuffer                             commandBuffer,
    const VkDebugMarkerMarkerInfoEXT*           pMarkerInfo)
{
    VkDevice device = s_cmdbuf_device_map[commandBuffer];

    if (s_device_dt[device].CmdDebugMarkerInsertEXT != nullptr)
    {
        s_device_dt[device].CmdDebugMarkerInsertEXT(commandBuffer, pMarkerInfo);
    }
}

// ----------------------------------------------------------------------------
// Layer glue code
// ----------------------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL GvEnumerateInstanceLayerProperties(uint32_t* pPropertyCount, VkLayerProperties* pProperties) 
{
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

VKAPI_ATTR VkResult VKAPI_CALL GvEnumerateInstanceExtensionProperties(const char* pLayerName, uint32_t* pPropertyCount, VkExtensionProperties* pProperties)
{
    // Inform the client that we have no extension properties if this layer
    // specifically is being queried.
    if (pLayerName != nullptr && strcmp(pLayerName, kLayerName) == 0) {
        if (pPropertyCount) *pPropertyCount = 0;
        return VK_SUCCESS;
    }

    // Vulkan spec mandates returning this when this layer isn't being queried.
    return VK_ERROR_LAYER_NOT_PRESENT;
}

VKAPI_ATTR VkResult VKAPI_CALL GvEnumeratePhysicalDevices(VkInstance instance, uint32_t* pPhysicalDeviceCount, VkPhysicalDevice* pPhysicalDevices)
{
    PFN_vkEnumeratePhysicalDevices fp_EnumeratePhysicalDevices = nullptr;
    {
        LocalGuard lock(s_layer_mutex);
        fp_EnumeratePhysicalDevices =
            s_instance_dt[instance].EnumeratePhysicalDevices;
    }
    VkResult result = fp_EnumeratePhysicalDevices(instance, pPhysicalDeviceCount, pPhysicalDevices);

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
VKAPI_ATTR VkResult VKAPI_CALL GvEnumerateDeviceLayerProperties(VkPhysicalDevice physicalDevice, uint32_t* pPropertyCount, VkLayerProperties* pProperties)
{
    // This function is supposed to return the same results as
    // EnumerateInstanceLayerProperties since device layers were deprecated.
    return GvEnumerateInstanceLayerProperties(pPropertyCount, pProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL GvEnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice, const char* pLayerName, uint32_t* pPropertyCount, VkExtensionProperties* pProperties)
{
    // If only our layer is being queried, just return our listing
    if (pLayerName != nullptr && strcmp(pLayerName, kLayerName) == 0) 
    {
        VkResult layerOnlyResult = VK_SUCCESS;

        if (nullptr == pProperties)
        {
            *pPropertyCount = s_numDeviceExtensions;
        }
        else
        {
            auto numExtensionsToCopy = s_numDeviceExtensions;
            if (*pPropertyCount < s_numDeviceExtensions)
            {
                numExtensionsToCopy = *pPropertyCount;
            }

            memcpy(pProperties, s_deviceExtensions, numExtensionsToCopy * sizeof(VkExtensionProperties));
            *pPropertyCount = numExtensionsToCopy;

            if (numExtensionsToCopy < s_numDeviceExtensions)
            {
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

    if (pLayerName != nullptr)
    {
        // if this is another layer, we can just return the data from it!
        return fp_EnumerateDeviceExtensionProperties(physicalDevice, pLayerName,
            pPropertyCount, pProperties);
    }

    uint32_t numOtherExtensions = 0;
    VkResult result = fp_EnumerateDeviceExtensionProperties(physicalDevice, nullptr, &numOtherExtensions, nullptr);
    if (result != VK_SUCCESS)
    {
        return result;
    }
    
    std::vector<VkExtensionProperties> extensions(numOtherExtensions);
    result = fp_EnumerateDeviceExtensionProperties(physicalDevice, nullptr, &numOtherExtensions, &extensions[0]);
    if (result != VK_SUCCESS)
    {
        return result;
    }

    // let's scan the list of extensions from down the chain, and add our unique extensions
    for (uint32_t extIdx = 0; extIdx < s_numDeviceExtensions; extIdx++)
    {
        auto curDeviceExt = s_deviceExtensions[extIdx];
        bool uniqueExtension = true;

        for (auto extProps : extensions)
        {
            if (0 == strcmp(extProps.extensionName, curDeviceExt.extensionName))
            {
                uniqueExtension = false;
                break;
            }
        }

        if (uniqueExtension)
        {
            extensions.push_back(curDeviceExt);
        }
    }

    if (nullptr == pProperties)
    {
        // just a count
        *pPropertyCount = uint32_t(extensions.size());
    }
    else
    {
        uint32_t numExtToCopy = uint32_t(extensions.size());
        if (numExtToCopy > *pPropertyCount)
        {
            numExtToCopy = *pPropertyCount;
        }

        for (uint32_t copyIdx = 0; copyIdx < numExtToCopy; copyIdx++)
        {
            pProperties[copyIdx] = extensions[copyIdx];
        }

        *pPropertyCount = numExtToCopy;

        if (numExtToCopy < extensions.size())
        {
            return VK_INCOMPLETE;
        }
    }
    
    return VK_SUCCESS;
}

// ----------------------------------------------------------------------------
// Layer bootstrapping code
// ----------------------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL GvCreateInstance(const VkInstanceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkInstance* pInstance)
{
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
    dispatch_table.GetInstanceProcAddr =
        (PFN_vkGetInstanceProcAddr)next_gipa(*pInstance, "vkGetInstanceProcAddr");
    dispatch_table.DestroyInstance =
        (PFN_vkDestroyInstance)next_gipa(*pInstance, "vkDestroyInstance");
    dispatch_table.EnumerateDeviceExtensionProperties =
        (PFN_vkEnumerateDeviceExtensionProperties)next_gipa(
            *pInstance, "vkEnumerateDeviceExtensionProperties");
    dispatch_table.EnumeratePhysicalDevices =
        (PFN_vkEnumeratePhysicalDevices)next_gipa(*pInstance,
            "vkEnumeratePhysicalDevices");

    {
        LocalGuard lock(s_layer_mutex);
        s_instance_dt[*pInstance] = dispatch_table;
    }

    //GPUVoyeur_inst.PostCallCreateInstance(pCreateInfo, pAllocator, pInstance);

    return result;
}

VKAPI_ATTR void VKAPI_CALL GvDestroyInstance(VkInstance instance, const VkAllocationCallbacks* pAllocator)
{
    LocalGuard lock(s_layer_mutex);
    s_instance_dt.erase(instance);
}

VKAPI_ATTR VkResult VKAPI_CALL GvCreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkDevice* pDevice)
{
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

    //GPUVoyeur_inst.PreCallCreateDevice(physicalDevice, pCreateInfo, pAllocator, pDevice);

    // Need to call vkCreateDevice down the chain to actually create the device
    PFN_vkCreateDevice createFunc =
        (PFN_vkCreateDevice)next_gipa(VK_NULL_HANDLE, "vkCreateDevice");
    VkResult result =
        createFunc(physicalDevice, pCreateInfo, pAllocator, pDevice);


    // TODO: Instead of the manual fetch of proc addresses, we might want to use the
    // functionality in LayerFactor/vk_dispatch_table_helper.h
    // We can fill in the dispatch_tables, and leverage it for both the forwarding
    // logic in the layer intercept points, and the layer's own usage of Vulkan APIs
    // in order to manage/use Vulkan stuff
    // This would modify the behavior of GvCreateInstance, GvCreateDevice, 
    // GvGetDispatchedInstanceProcAddr and GvGetDispatchedDeviceProcAddr.
    VkLayerDispatchTable dispatch_table = {};
    dispatch_table.GetDeviceProcAddr =
        (PFN_vkGetDeviceProcAddr)next_gdpa(*pDevice, "vkGetDeviceProcAddr");
    dispatch_table.DestroyDevice =
        (PFN_vkDestroyDevice)next_gdpa(*pDevice, "vkDestroyDevice");
    dispatch_table.CreateCommandPool =
        (PFN_vkCreateCommandPool)next_gdpa(*pDevice, "vkCreateCommandPool");
    dispatch_table.DestroyCommandPool =
        (PFN_vkDestroyCommandPool)next_gdpa(*pDevice, "vkDestroyCommandPool");
    dispatch_table.ResetCommandPool =
        (PFN_vkResetCommandPool)next_gdpa(*pDevice, "vkResetCommandPool");
    dispatch_table.AllocateCommandBuffers =
        (PFN_vkAllocateCommandBuffers)next_gdpa(*pDevice, "vkAllocateCommandBuffers");
    dispatch_table.FreeCommandBuffers =
        (PFN_vkFreeCommandBuffers)next_gdpa(*pDevice, "vkFreeCommandBuffers");
    dispatch_table.BeginCommandBuffer =
        (PFN_vkBeginCommandBuffer)next_gdpa(*pDevice, "vkBeginCommandBuffer");
    dispatch_table.EndCommandBuffer =
        (PFN_vkEndCommandBuffer)next_gdpa(*pDevice, "vkEndCommandBuffer");
    dispatch_table.ResetCommandBuffer =
        (PFN_vkResetCommandBuffer)next_gdpa(*pDevice, "vkResetCommandBuffer");
    dispatch_table.GetDeviceQueue =
        (PFN_vkGetDeviceQueue)next_gdpa(*pDevice, "vkGetDeviceQueue");
    dispatch_table.QueueSubmit =
        (PFN_vkQueueSubmit)next_gdpa(*pDevice, "vkQueueSubmit");

    dispatch_table.QueuePresentKHR =
        (PFN_vkQueuePresentKHR)next_gdpa(*pDevice, "vkQueuePresentKHR");
    dispatch_table.AcquireNextImageKHR =
        (PFN_vkAcquireNextImageKHR)next_gdpa(*pDevice, "vkAcquireNextImageKHR");

    dispatch_table.DebugMarkerSetObjectTagEXT =
        (PFN_vkDebugMarkerSetObjectTagEXT)next_gdpa(*pDevice, "vkDebugMarkerSetObjectTagEXT");
    dispatch_table.DebugMarkerSetObjectNameEXT =
        (PFN_vkDebugMarkerSetObjectNameEXT)next_gdpa(*pDevice, "vkDebugMarkerSetObjectTagEXT");
    dispatch_table.CmdDebugMarkerBeginEXT =
        (PFN_vkCmdDebugMarkerBeginEXT)next_gdpa(*pDevice, "vkCmdDebugMarkerBeginEXT");
    dispatch_table.CmdDebugMarkerEndEXT =
        (PFN_vkCmdDebugMarkerEndEXT)next_gdpa(*pDevice, "vkCmdDebugMarkerEndEXT");
    dispatch_table.CmdDebugMarkerInsertEXT =
        (PFN_vkCmdDebugMarkerInsertEXT)next_gdpa(*pDevice, "vkCmdDebugMarkerInsertEXT");

    {
        LocalGuard lock(s_layer_mutex);
        s_device_dt[*pDevice] = dispatch_table;
        s_numDevices++;
        if (1 == s_numDevices)
        {
            s_global_dispatch_table = &s_device_dt[*pDevice];
        }
    }

    // speed-up hacks
    if ((dispatch_table.DebugMarkerSetObjectTagEXT != nullptr) ||
        (dispatch_table.DebugMarkerSetObjectNameEXT != nullptr) ||
        (dispatch_table.CmdDebugMarkerBeginEXT != nullptr) ||
        (dispatch_table.CmdDebugMarkerEndEXT != nullptr) ||
        (dispatch_table.CmdDebugMarkerInsertEXT != nullptr))
    {
        // init to false
        s_downstreamDebugMarker = true;
    }

    //GPUVoyeur_inst.PostCallCreateDevice(physicalDevice, pCreateInfo, pAllocator, pDevice);

    return result;
}

VKAPI_ATTR void VKAPI_CALL GvDestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator)
{
    LocalGuard lock(s_layer_mutex);
    s_device_dt.erase(device);
}

#define GV_GETPROCADDR(func) \
  if (strcmp(pName, "vk" #func) == 0) return (PFN_vkVoidFunction)&Gv##func;


// GetDeviceProcAddr is declared before GetInstanceProcAddr because otherwise we'd
// need a forward declaration of GvGetDeviceProcAddr -_-
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GvGetDeviceProcAddr(VkDevice device, const char* pName)
{
    // Functions available through GetInstanceProcAddr and GetDeviceProcAddr
    GV_GETPROCADDR(GetDeviceProcAddr);
    GV_GETPROCADDR(EnumerateDeviceLayerProperties);
    GV_GETPROCADDR(EnumerateDeviceExtensionProperties);
    GV_GETPROCADDR(CreateDevice);
    GV_GETPROCADDR(DestroyDevice);
    GV_GETPROCADDR(CreateCommandPool);
    GV_GETPROCADDR(DestroyCommandPool);
    GV_GETPROCADDR(ResetCommandPool);
    GV_GETPROCADDR(AllocateCommandBuffers);
    GV_GETPROCADDR(FreeCommandBuffers);
    GV_GETPROCADDR(BeginCommandBuffer);
    GV_GETPROCADDR(EndCommandBuffer);
    GV_GETPROCADDR(ResetCommandBuffer);
    GV_GETPROCADDR(GetDeviceQueue);
    GV_GETPROCADDR(QueueSubmit);

    GV_GETPROCADDR(QueuePresentKHR);
    GV_GETPROCADDR(AcquireNextImageKHR);

    GV_GETPROCADDR(DebugMarkerSetObjectTagEXT);
    GV_GETPROCADDR(DebugMarkerSetObjectNameEXT);
    GV_GETPROCADDR(CmdDebugMarkerBeginEXT);
    GV_GETPROCADDR(CmdDebugMarkerEndEXT);
    GV_GETPROCADDR(CmdDebugMarkerInsertEXT);

    {
        LocalGuard lock(s_layer_mutex);
        return s_device_dt[device].GetDeviceProcAddr(device, pName);
    }
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GvGetInstanceProcAddr(VkInstance instance, const char* pName) 
{
    // Functions available only through GetInstanceProcAddr
    GV_GETPROCADDR(GetInstanceProcAddr);
    GV_GETPROCADDR(CreateInstance);
    GV_GETPROCADDR(DestroyInstance);
    GV_GETPROCADDR(EnumerateInstanceLayerProperties);
    GV_GETPROCADDR(EnumerateInstanceExtensionProperties);
    GV_GETPROCADDR(EnumeratePhysicalDevices);

    // Functions available through GetInstanceProcAddr and GetDeviceProcAddr
    GV_GETPROCADDR(GetDeviceProcAddr);
    GV_GETPROCADDR(EnumerateDeviceLayerProperties);
    GV_GETPROCADDR(EnumerateDeviceExtensionProperties);
    GV_GETPROCADDR(CreateDevice);
    GV_GETPROCADDR(DestroyDevice);
    GV_GETPROCADDR(CreateCommandPool);
    GV_GETPROCADDR(DestroyCommandPool);
    GV_GETPROCADDR(ResetCommandPool);
    GV_GETPROCADDR(AllocateCommandBuffers);
    GV_GETPROCADDR(FreeCommandBuffers);
    GV_GETPROCADDR(BeginCommandBuffer);
    GV_GETPROCADDR(EndCommandBuffer);
    GV_GETPROCADDR(ResetCommandBuffer);
    GV_GETPROCADDR(GetDeviceQueue);
    GV_GETPROCADDR(QueueSubmit);

    GV_GETPROCADDR(QueuePresentKHR);
    GV_GETPROCADDR(AcquireNextImageKHR);

    GV_GETPROCADDR(DebugMarkerSetObjectTagEXT);
    GV_GETPROCADDR(DebugMarkerSetObjectNameEXT);
    GV_GETPROCADDR(CmdDebugMarkerBeginEXT);
    GV_GETPROCADDR(CmdDebugMarkerEndEXT);
    GV_GETPROCADDR(CmdDebugMarkerInsertEXT);

    {
        LocalGuard lock(s_layer_mutex);
        return s_instance_dt[instance].GetInstanceProcAddr(instance, pName);
    }
}

#undef GV_GETPROCADDR

// TODO: Not clear if we really need the __declspec for Windows
// The linker complains about functions being exported multiple times
// but if we don't use declspec, the functions aren't latched by the loader.
#if defined(WIN32)
#define GPU_VOYEUR_EXPORT extern "C" __declspec(dllexport) VK_LAYER_EXPORT
#else
#define GPU_VOYEUR_EXPORT extern "C" VK_LAYER_EXPORT
#endif

// This is only needed on pre-1.1.82 Vulkan loaders
// For whatever reason, the loader looks for vkCreateDevice from the exported
// GetInstanceProcAddr instead of what is specified in NegotiateLoaderLayerInterfaceVersion.
//#define LOADER_PROC_HACK

#if defined(LOADER_PROC_HACK)
GPU_VOYEUR_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice dev, const char *funcName) {
    return GV::GvGetDeviceProcAddr(dev, funcName);
}

GPU_VOYEUR_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *funcName) {
    return GV::GvGetInstanceProcAddr(instance, funcName);
}
#endif

// layer export
GPU_VOYEUR_EXPORT VKAPI_ATTR VkResult VKAPI_CALL GvNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* pVersionStruct)
{
    if (pVersionStruct == NULL ||
        pVersionStruct->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    // We don't support older interface versions.
    if (pVersionStruct->loaderLayerInterfaceVersion <
        kLoaderLayerInterfaceVersion) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    else {
        pVersionStruct->loaderLayerInterfaceVersion = kLoaderLayerInterfaceVersion;
    }

    pVersionStruct->pfnGetInstanceProcAddr = &GvGetInstanceProcAddr;
    pVersionStruct->pfnGetDeviceProcAddr = &GvGetDeviceProcAddr;
    // This is null because we have no physical device extensions
    pVersionStruct->pfnGetPhysicalDeviceProcAddr = nullptr;

    return VK_SUCCESS;
}

#undef GPU_VOYEUR_EXPORT

} // namespace GV


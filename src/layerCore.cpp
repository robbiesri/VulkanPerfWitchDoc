#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include "vk_layer_dispatch_table.h"

#include <mutex>
#include <unordered_map>

#include "WitchDoc.h"
#include "layerCore.h"

namespace GWD {

// layer metadata

static constexpr const char* const kGwdVersion = "0.1";

static constexpr uint32_t kLoaderLayerInterfaceVersion = 2;

static constexpr const char* const kLayerName =
    "VK_LAYER_GOOGLE_witch_doctor";
static constexpr const char* const kLayerDescription =
    "Google's magical GPU performance layer for (mostly) Stadia";

static constexpr const uint32_t kLayerImplVersion = 1;
static constexpr const uint32_t kLayerSpecVersion = VK_API_VERSION_1_1;

// TODO: Add support for VK_EXT_debug_utils (BLEH)
//static const VkExtensionProperties s_deviceExtensions[1] = { { VK_EXT_DEBUG_MARKER_EXTENSION_NAME, VK_EXT_DEBUG_MARKER_SPEC_VERSION } };
//static const uint32_t s_numDeviceExtensions = uint32_t(sizeof(s_deviceExtensions) / sizeof(VkExtensionProperties));
static const VkExtensionProperties s_deviceExtensions[1] = { };
static const uint32_t s_numDeviceExtensions = 0;

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

typedef void *dispatch_key;
static inline dispatch_key get_dispatch_key(const void *object) { return (dispatch_key) * (VkLayerDispatchTable **)object; }

static WitchDoctor WitchDoc_inst;

// Layer helper for external clients to dispatch
PFN_vkVoidFunction GwdGetDispatchedDeviceProcAddr(VkDevice device, const char* pName)
{
    LocalGuard lock(s_layer_mutex);
    return s_device_dt[device].GetDeviceProcAddr(device, pName);
}

PFN_vkVoidFunction GwdGetDispatchedInstanceProcAddr(VkInstance instance, const char* pName)
{
    LocalGuard lock(s_layer_mutex);
    return s_instance_dt[instance].GetInstanceProcAddr(instance, pName);
}

// ----------------------------------------------------------------------------
// Core layer logic
// ----------------------------------------------------------------------------


// TODO: For VkCommandBuffer and VkQueue, we store a map between VkCommandBuffer and VkQueue to
// VkDevice. We might want to see if we can use the same mechanism as the dispatch_key from layer_factory

VKAPI_ATTR void VKAPI_CALL GwdGetDeviceQueue(
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

VKAPI_ATTR VkResult VKAPI_CALL GwdQueueSubmit(
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

// ----------------------------------------------------------------------------
// Layer glue code
// ----------------------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL GwdEnumerateInstanceLayerProperties(uint32_t* pPropertyCount, VkLayerProperties* pProperties) 
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

VKAPI_ATTR VkResult VKAPI_CALL GwdEnumerateInstanceExtensionProperties(const char* pLayerName, uint32_t* pPropertyCount, VkExtensionProperties* pProperties)
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

VKAPI_ATTR VkResult VKAPI_CALL GwdEnumeratePhysicalDevices(VkInstance instance, uint32_t* pPhysicalDeviceCount, VkPhysicalDevice* pPhysicalDevices)
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
VKAPI_ATTR VkResult VKAPI_CALL GwdEnumerateDeviceLayerProperties(VkPhysicalDevice physicalDevice, uint32_t* pPropertyCount, VkLayerProperties* pProperties)
{
    // This function is supposed to return the same results as
    // EnumerateInstanceLayerProperties since device layers were deprecated.
    return GwdEnumerateInstanceLayerProperties(pPropertyCount, pProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL GwdEnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice, const char* pLayerName, uint32_t* pPropertyCount, VkExtensionProperties* pProperties)
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

            if (numExtensionsToCopy > 0)
            {
                memcpy(pProperties, s_deviceExtensions, numExtensionsToCopy * sizeof(VkExtensionProperties));
            }
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

VKAPI_ATTR VkResult VKAPI_CALL GwdCreateInstance(const VkInstanceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkInstance* pInstance)
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

    WitchDoc_inst.PostCallCreateInstance(pCreateInfo, pAllocator, pInstance);

    return result;
}

VKAPI_ATTR void VKAPI_CALL GwdDestroyInstance(VkInstance instance, const VkAllocationCallbacks* pAllocator)
{
    LocalGuard lock(s_layer_mutex);
    s_instance_dt.erase(instance);

    // TODO: Call down the chain??
}

VKAPI_ATTR VkResult VKAPI_CALL GwdCreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkDevice* pDevice)
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
    // This would modify the behavior of GwdCreateInstance, GwdCreateDevice, 
    // GwdGetDispatchedInstanceProcAddr and GwdGetDispatchedDeviceProcAddr.

    VkLayerDispatchTable dispatch_table = {};
    dispatch_table.GetDeviceProcAddr =
        (PFN_vkGetDeviceProcAddr)next_gdpa(*pDevice, "vkGetDeviceProcAddr");
    dispatch_table.DestroyDevice =
        (PFN_vkDestroyDevice)next_gdpa(*pDevice, "vkDestroyDevice");
    dispatch_table.GetDeviceQueue =
        (PFN_vkGetDeviceQueue)next_gdpa(*pDevice, "vkGetDeviceQueue");
    dispatch_table.QueueSubmit =
        (PFN_vkQueueSubmit)next_gdpa(*pDevice, "vkQueueSubmit");

    {
        LocalGuard lock(s_layer_mutex);
        s_device_dt[*pDevice] = dispatch_table;
        s_numDevices++;
        if (1 == s_numDevices)
        {
            s_global_dispatch_table = &s_device_dt[*pDevice];
        }
    }

    WitchDoc_inst.PostCallCreateDevice(physicalDevice, pCreateInfo, pAllocator, pDevice);

    return result;
}

VKAPI_ATTR void VKAPI_CALL GwdDestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator)
{
    LocalGuard lock(s_layer_mutex);
    s_device_dt.erase(device);
}

#define GWD_GETPROCADDR(func) \
  if (strcmp(pName, "vk" #func) == 0) return (PFN_vkVoidFunction)&Gwd##func;


// GetDeviceProcAddr is declared before GetInstanceProcAddr because otherwise we'd
// need a forward declaration of GwdGetDeviceProcAddr -_-
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GwdGetDeviceProcAddr(VkDevice device, const char* pName)
{
    // Functions available through GetInstanceProcAddr and GetDeviceProcAddr
    GWD_GETPROCADDR(GetDeviceProcAddr);
    GWD_GETPROCADDR(EnumerateDeviceLayerProperties);
    GWD_GETPROCADDR(EnumerateDeviceExtensionProperties);
    GWD_GETPROCADDR(CreateDevice);
    GWD_GETPROCADDR(DestroyDevice);
    GWD_GETPROCADDR(GetDeviceQueue);
    GWD_GETPROCADDR(QueueSubmit);

    {
        LocalGuard lock(s_layer_mutex);
        return s_device_dt[device].GetDeviceProcAddr(device, pName);
    }
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GwdGetInstanceProcAddr(VkInstance instance, const char* pName) 
{
    // Functions available only through GetInstanceProcAddr
    GWD_GETPROCADDR(GetInstanceProcAddr);
    GWD_GETPROCADDR(CreateInstance);
    GWD_GETPROCADDR(DestroyInstance);
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
// GetInstanceProcAddr instead of what is specified in NegotiateLoaderLayerInterfaceVersion.
//#define LOADER_PROC_HACK

#if defined(LOADER_PROC_HACK)
WITCH_DOCTOR_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice dev, const char *funcName) {
    return GWD::GwdGetDeviceProcAddr(dev, funcName);
}

WITCH_DOCTOR_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *funcName) {
    return GWD::GwdGetInstanceProcAddr(instance, funcName);
}
#endif

// layer export
WITCH_DOCTOR_EXPORT VKAPI_ATTR VkResult VKAPI_CALL gwdNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* pVersionStruct)
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

    pVersionStruct->pfnGetInstanceProcAddr = &GwdGetInstanceProcAddr;
    pVersionStruct->pfnGetDeviceProcAddr = &GwdGetDeviceProcAddr;
    // This is null because we have no physical device extensions
    pVersionStruct->pfnGetPhysicalDeviceProcAddr = nullptr;

    return VK_SUCCESS;
}

#undef WITCH_DOCTOR_EXPORT

} // namespace GWD


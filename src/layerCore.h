#pragma once

#include <vulkan/vk_layer.h>

namespace GWD 
{

// Helper functions to grab the next proc address in the dispatch chain
PFN_vkVoidFunction GwdGetDispatchedDeviceProcAddr(VkDevice device, const char* pName);
PFN_vkVoidFunction GwdGetDispatchedInstanceProcAddr(VkInstance instance, const char* pName);

}
#pragma once

#include <vulkan/vk_layer.h>

namespace GV 
{

// Helper functions to grab the next proc address in the dispatch chain
PFN_vkVoidFunction GvGetDispatchedDeviceProcAddr(VkDevice device, const char* pName);
PFN_vkVoidFunction GvGetDispatchedInstanceProcAddr(VkInstance instance, const char* pName);

}
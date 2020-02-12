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

#include <vulkan/vk_layer.h>

namespace GWDInterface {

// Helper functions to grab the next proc address in the dispatch chain
PFN_vkVoidFunction GwdGetDispatchedDeviceProcAddr(VkDevice device,
                                                  const char* pName);
PFN_vkVoidFunction GwdGetDispatchedInstanceProcAddr(VkInstance instance,
                                                    const char* pName);

}  // namespace GWDInterface

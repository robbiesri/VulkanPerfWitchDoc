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

#include <iostream>

namespace GWD {

WitchDoctor::WitchDoctor() {}

WitchDoctor::~WitchDoctor() {}

void WitchDoctor::PerformanceWarningMessage(std::string& message) {
  if (m_debug_utils_messengers.size() > 0) {
    std::lock_guard<std::mutex> lock(m_debug_utils_messenger_mutex);
    VkDebugUtilsMessengerCallbackDataEXT callback_data = {};
    callback_data.sType =
        VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT;
    callback_data.pMessage = message.c_str();

    for (const auto& messenger : m_debug_utils_messengers) {
      messenger.second.pfnUserCallback(
          VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT,
          VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT, &callback_data,
          messenger.second.pUserData);
    }
  } else {
#if defined(WIN32)
    OutputDebugString(message.c_str());
    OutputDebugString("\n");
#else   // defined(WIN32)
    std::cout << message.c_str() << std::endl;
#endif  // defined(WIN32)
  }
}

}  // namespace GWD

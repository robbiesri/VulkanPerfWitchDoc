cmake_minimum_required(VERSION 3.0)

project(StadiaPerfLayer)

# Is this what conflicts with the manual layer exports?
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

set(VULKAN_DIR_FOUND FALSE)
if (NOT VULKAN_DIR)
  set(VULKAN_DIR $ENV{VULKAN_SDK})
endif()
if (NOT "${VULKAN_DIR}" STREQUAL "")
  message(STATUS "Using Vulkan found at ${VULKAN_DIR}")
  set(VULKAN_DIR_FOUND TRUE)
endif()

if ((WIN32 OR UNIX))
    if (NOT VULKAN_DIR_FOUND)
        message( FATAL_ERROR "Vulkan SDK could not be located" )
    endif()
endif()

if(WIN32)
    set(VULKAN_INCLUDE_DIR  ${VULKAN_DIR}/include)
    set(VULKAN_LIBRARY  ${VULKAN_DIR}/lib/vulkan-1.lib)
elseif(GGP)
    set(VULKAN_INCLUDE_DIR  ${GGP_SYSROOT_PATH}/usr/include)
    set(VULKAN_LIBRARY  ${GGP_SYSROOT_PATH}/lib/libvulkan.so)
elseif(UNIX AND NOT APPLE)
    set(VULKAN_INCLUDE_DIR  ${VULKAN_DIR}/include)
    set(VULKAN_LIBRARY  ${VULKAN_DIR}/lib/libvulkan.so)
endif()

set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)

add_subdirectory(src)
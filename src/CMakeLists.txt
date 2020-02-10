cmake_minimum_required(VERSION 3.0)

set(target_name StadiaPerfLayer)

add_library(${target_name} SHARED 
                                  #${CMAKE_CURRENT_SOURCE_DIR}/GPUVoyeur.h
                                  #${CMAKE_CURRENT_SOURCE_DIR}/GPUVoyeur.cpp
                                  ${CMAKE_CURRENT_SOURCE_DIR}/layerCore.h
                                  ${CMAKE_CURRENT_SOURCE_DIR}/layerCore.cpp
                  )
                  
# visual studio stuff                  

target_include_directories(${target_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
                                                  ${VULKAN_DIR}/include
                                                  ${VULKAN_DIR}/LayerFactory/Project)


file(TO_NATIVE_PATH ${CMAKE_SOURCE_DIR}/bin/${CMAKE_SYSTEM_NAME} BIN_DIR)
file(MAKE_DIRECTORY ${BIN_DIR})
 add_custom_command(TARGET ${target_name} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
  $<TARGET_FILE:${target_name}>
  ${BIN_DIR}
  COMMENT "Copying ${target_name} to ${BIN_DIR}"
)

message(STATUS "Generating post-build command to copy JSON to bin")

file(TO_NATIVE_PATH ${CMAKE_SOURCE_DIR}/resources/${CMAKE_SYSTEM_NAME}/VkLayer_GPUVoyeur_explicit.json SOURCE_LAYER_JSON)
file(TO_NATIVE_PATH ${BIN_DIR}/VkLayer_GPUVoyeur.json DEST_LAYER_JSON)

add_custom_command(TARGET ${target_name} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
    ${SOURCE_LAYER_JSON}
    ${DEST_LAYER_JSON}
    COMMENT "Copying explicit layer JSON to ${BIN_DIR}"
)

if(CMAKE_SYSTEM_NAME STREQUAL "Windows")

    target_link_libraries(${target_name} PUBLIC ws2_32.lib)

    add_definitions(-DVK_USE_PLATFORM_WIN32_KHR -DWIN32_LEAN_AND_MEAN -D_CRT_SECURE_NO_WARNINGS -D_USE_MATH_DEFINES)
        
    #remove /Og from builds (default as par of O2, so specify all desired optimizations)
    string(REPLACE "/O2" "/Od /Oi /Ot /Oy /Ob2 /Gs /GF /Gy" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")	
        
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /Zi")
    set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${CMAKE_SHARED_LINKER_FLAGS_RELEASE} /DEBUG /OPT:REF /OPT:ICF")

    file(TO_NATIVE_PATH $(OutDir)${target_name}.pdb PDB_SRC)
    file(TO_NATIVE_PATH ${BIN_DIR}/${target_name}.pdb PDB_DST)

    add_custom_command(TARGET ${target_name} POST_BUILD   
        COMMAND echo "Copy ${PDB_SRC} ${PDB_DST}"
        COMMAND copy ${PDB_SRC} ${PDB_DST}
    )

elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")

  set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++14 -Wpointer-arith -Wno-unused-function -Wno-sign-compare")
  set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wpointer-arith -Wno-unused-function -Wno-sign-compare")
  set_target_properties(${target_name} PROPERTIES LINK_FLAGS "-Wl,-Bsymbolic,--exclude-libs,ALL")

endif()

target_compile_definitions(${target_name} PRIVATE VK_PROTOTYPES API_NAME="Vulkan")